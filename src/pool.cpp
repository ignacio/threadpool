/*!
 * \file pool.cpp
 * \author ichramm
 * \date June 30, 2012, 2:43 AM
 *
 * Thread Pool class implementation
 */
#include "../pool.h"

#if _MSC_VER > 1000
# pragma warning(push)
// this warning shows when using this_thread::sleep()
# pragma warning(disable: 4244) //warning C4244: 'argument' : conversion from '__int64' to 'long', possible loss of data
#endif
#include <boost/bind.hpp>
#include <boost/weak_ptr.hpp>
#include <boost/shared_ptr.hpp>
#include <boost/make_shared.hpp>
#include <boost/thread.hpp>
#include <boost/thread/condition.hpp>
#include <boost/detail/atomic_count.hpp>
#if _MSC_VER > 1000
# pragma warning(pop)
#endif

#include <string>
#include <queue>

using namespace std;
using namespace boost;

typedef boost::detail::atomic_count atomic_counter;

namespace threadpool
{

static const system_time invalid_system_time;

/*! Time to sleep to avoid 100% CPU usage */
static const posix_time::milliseconds worker_idle_time(2);


struct pool::impl
{
private:

	/*!
	 * Internal flags used by the pool monitor
	 */
	enum resize_flags
	{
		flag_no_resize,
		flag_resize_up,
		flag_resize_down
	};

	/*!
	 * Task object with schedule information
	 */
	class task_impl
	{
	private:
		task_type   m_task;
		system_time m_schedule;

	public:
		task_impl(task_type task = 0, system_time schedule = invalid_system_time)
		 : m_task(task)
		 , m_schedule(schedule)
		{
		}

		bool is_on_schedule() const
		{
			return (m_schedule.is_not_a_date_time() || m_schedule <= get_system_time());
		}

		void operator()()
		{
			m_task();
		}
	};

	/*!
	 * This struct holds the proper thread object and a Boolean value indicating whether
	 * the thread is busy executing a task or not.
	 * The Boolean value should be checked each time a task is done in order
	 * for the thread to stop.
	 *
	 * Objects of this type shall always be used with the lock acquired
	 */
	struct pool_thread : public enable_shared_from_this<pool_thread>
	{
		typedef shared_ptr<pool_thread> ptr;
		typedef function<void(pool_thread*)>     worker;

		bool     m_busy;   /*!< Indicates whether the worker is executing a task */
		thread*  m_thread; /*!< The thread object itself */

		/*!
		 * Initializes this, creates the thread and subsequently starts the worker
		 *
		 * \param worker Function to execute in the thread
		 */
		pool_thread(worker work)
		 : m_busy(true)
		{ // avoid using this in member initializer list 
			m_thread = new thread(work, this);
		}

		/*!
		 * Destroys the thread, waits until the thread ends
		 */
		~pool_thread()
		{
			m_thread->join();
			delete m_thread;
		}

		/*!
		 * Set when a thread is done waiting for call call
		 */
		void set_busy(bool b)
		{
			m_busy = b;
		}

		/*!
		 * Use to ask whether a thread is executing a task or waiting for a new one
		 */
		bool is_busy() const
		{
			return m_busy;
		}

		/*!
		 * Send interrupt signal to the thread
		 * Threads in idle state (those for \c is_busy will return \c false) should
		 * end (almost) immediately
		 */
		void interrupt()
		{
			m_thread->interrupt();
		}

		/*!
		 * Waits until the thread ends
		 */
		void join()
		{
			m_thread->join();
		}
	};

	/*!
	 * This function computes the starting (and minimum) size of the pool, given the constructor parameters
	 */
	static inline unsigned int compute_min_threads(unsigned int desired_min_threads, unsigned int desired_max_threads)
	{
		if ( desired_min_threads == unsigned(-1) )
		{
			unsigned int candidate_thread_count = thread::hardware_concurrency() * 2;
			if ( candidate_thread_count == 0 )
			{ // information not available, create at least one thread
				candidate_thread_count = 1;
			}
			desired_min_threads = std::min(candidate_thread_count, desired_max_threads);
		}
		return desired_min_threads == desired_max_threads ? desired_min_threads : desired_min_threads + 1;
	}

	volatile bool      m_stopPool;             /*!< Set when the pool is being destroyed */
	const unsigned int m_minThreads;           /*!< Minimum thread count */
	const unsigned int m_maxThreads;           /*!< Maximum thread count */
	const unsigned int m_resizeUpTolerance;    /*!< Milliseconds to wait before creating more threads */
	const unsigned int m_resizeDownTolerance;  /*!< Milliseconds to wait before deleting threads */
	shutdown_option    m_onShutdown;           /*!< How to behave on destruction */
	atomic_counter     m_activeTasks;          /*!< Number of active tasks */
	atomic_counter     m_threadCount;          /*!< Number of threads in the pool \see http://stackoverflow.com/questions/228908/is-listsize-really-on */

	mutex              m_tasksMutex;           /*!< Synchronizes access to the task queue */
	mutex              m_threadsMutex;         /*!< Synchronizes access to the pool */
	condition          m_tasksCondition;       /*!< Condition to notify when a new task arrives  */
	condition          m_monitorCondition;     /*!< Condition to notify the monitor when it has to stop */

	queue<task_impl>       m_pendingTasks;     /*!< Task queue */
	list<pool_thread::ptr> m_threads;          /*!< List of threads */

public:

	/*!
	 *
	 */
	impl(unsigned int min_threads, unsigned int max_threads, unsigned int timeout_add_threads,
	     unsigned int timeout_del_threads, shutdown_option on_shutdown)
	 : m_stopPool(false)
	 , m_minThreads(compute_min_threads(min_threads, max_threads))
	 , m_maxThreads(max_threads) // cannot use more than max_threads threads
	 , m_resizeUpTolerance(timeout_add_threads)
	 , m_resizeDownTolerance(timeout_del_threads)
	 , m_onShutdown(on_shutdown)
	 , m_activeTasks(0)
	 , m_threadCount(0)
	{
		assert(m_maxThreads >= m_minThreads);

		while( pool_size() < m_minThreads )
		{
			add_thread();
		}

		if ( m_minThreads < m_maxThreads )
		{ // monitor only when the pool can actually be resized
			schedule(bind(&impl::pool_monitor, this), invalid_system_time);
		}
	}

	/*!
	 * Cancels all pending tasks
	 * Destroys the thread pool waiting for all threads to finish
	 */
	~impl()
	{
		m_stopPool = true;

		if ( m_minThreads < m_maxThreads )
		{ // wake up the monitor
			m_monitorCondition.notify_one();

			// wait until the monitor releases the lock
			lock_guard<mutex> lock(m_threadsMutex);
		}

		if ( m_onShutdown == shutdown_option_cancel_tasks )
		{
			lock_guard<mutex> lock(m_tasksMutex);
			while ( !m_pendingTasks.empty() ) {
				m_pendingTasks.pop();
			}
			// wake up all threads
			m_tasksCondition.notify_all();
		}
		else
		{ // m_onShutdown == shutdown_option_wait_for_tasks
			while ( active_tasks() > 0 || pending_tasks() > 0 )
			{ // make sure there are no tasks running and/or pending
				this_thread::sleep(worker_idle_time);
			}
		}

		while ( pool_size() > 0 )
		{ // there is no need to lock here
			remove_thread();
		}
	}

	/*!
	 * Schedules a task for execution
	 */
	void schedule(const task_type& task, const system_time& abs_time = invalid_system_time)
	{
		assert(task != 0);

		lock_guard<mutex> lock(m_tasksMutex);

		if ( m_stopPool )
		{
			return;
		}

		m_pendingTasks.push(task_impl(task, abs_time));

		//wake up only one thread
		m_tasksCondition.notify_one();
	}

	/*!
	 * Returns the number of active tasks. \see worker_thread
	 */
	unsigned int active_tasks()
	{
		return m_activeTasks;
	}

	/*!
	 * Return the number of pending tasks, aka the number
	 * of tasks in the queue
	 */
	unsigned int pending_tasks()
	{
		lock_guard<mutex> lock(m_tasksMutex);
		return m_pendingTasks.size();
	}

	/*!
	 * Returns the number of threads in the pool
	 *
	 * We use a separate counter because list::size is slow
	 */
	unsigned int pool_size()
	{
		return m_threadCount;
	}

private:

	/*!
	 * Adds a new thread to the pool
	 */
	void add_thread()
	{
		pool_thread::ptr t = make_shared<pool_thread>(bind(&impl::worker_thread, this, _1));

		m_threads.push_back(t);
		++m_threadCount;
	}

	/*!
	 * Removes a thread from the pool, possibly waiting until it ends
	 */
	void remove_thread()
	{
		pool_thread::ptr t = m_threads.back();
		m_threads.pop_back();

		--m_threadCount;
		t->interrupt();
		t->join();
	}

	/*!
	 * Removes \p count idle threads from the pool
	 * This function must be called with \c m_threadsMutex locked
	 */
	void remove_idle_threads(unsigned int count)
	{ // this function is called locked

		list<pool_thread::ptr>::iterator it = m_threads.begin();
		while ( it != m_threads.end() && count > 0 )
		{
			pool_thread::ptr &th = *it;

			{
				// don't let it take another task
				lock_guard<mutex> lock(m_tasksMutex);

				if ( th->is_busy() )
				{ // it is executing a task, or at least is not waiting for a new one
					it++;
					continue;
				}

				// it's waiting, will throw thread_interrupted
				th->interrupt();
			}

			--count;
			--m_threadCount;
			th->join();
			it = m_threads.erase(it);
		}
	}

	/*!
	 * This function loops forever polling the task queue
	 * for a task to execute.
	 *
	 * The function exists when the thread has been canceled or
	* when the pool is stopping. It holds a reference
	* to \c pool_thread::ptr so destruction is done safely
	 *
	 * \li Threads are canceled by the pool monitor
	 * \li The pool stops when it's being destroyed
	 */
	void worker_thread(pool_thread *t)
	{
		task_impl task;

		for( ; ; )
		{
			{
				mutex::scoped_lock lock(m_tasksMutex);

				if ( m_stopPool )
				{ // check before doing anything
					return;
				}

				// wait inside loop to cope with spurious wakes, see http://goo.gl/Oxv6T
				while( m_pendingTasks.empty() )
				{
					try
					{
						t->set_busy(false);
						m_tasksCondition.wait(lock);
						t->set_busy(true);
					}
					catch ( const thread_interrupted& )
					{ // thread has been canceled
						return;
					}

					if ( m_stopPool )
					{ // stop flag was set while waiting
						return;
					}
				}

				task = m_pendingTasks.front();
				m_pendingTasks.pop();

				if (task.is_on_schedule() == false)
				{  // the task is not yet ready to execute, it must be re-queued
					m_pendingTasks.push(task);
					// sleep the thread for a small amount of time in order to avoid stressing the CPU
					m_tasksCondition.timed_wait(lock, worker_idle_time);
					continue; // while(true)
				}
			}

			// disable interruption for this thread
			this_thread::disable_interruption di;

			++m_activeTasks;
			task();
			--m_activeTasks;

			// check if interruption has been requested before checking for new tasks
			// this should happen only when the pool is stopping
			if ( this_thread::interruption_requested() )
			{
				return;
			}
		}
	}

	/*!
	 * This function monitors the pool status in order to add or
	 * remove threads depending on the load.
	 *
	 *  If the pool is full and there are queued tasks then more
	 * threads are added to the pool. No threads are created until
	 * a configurable period of heavy load has passed.
	 *
	 *  If the pool is idle then threads are removed from the pool,
	 * but this time the waiting period is longer, it helps to
	 * avoid adding and removing threads all time.
	 *  The period to wait until the pool size is decreased is far
	 * longer than the period to wait until increasing the
	 * pool size.
	 */
	void pool_monitor()
	{
		static const float RESIZE_UP_FACTOR    = 1.5; // size increase factor
		static const float RESIZE_DOWN_FACTOR  = 2.0; // size decrease factor

		// milliseconds to sleep between checks
		static const posix_time::milliseconds THREAD_SLEEP_MS(1);

		const unsigned int MAX_STEPS_UP  = max(m_resizeUpTolerance, 2u); // make at least 2 steps
		const unsigned int MAX_STEPS_DOWN = max(m_resizeDownTolerance, 2u); // can wait, make at least 2 steps

		resize_flags resize_flag = flag_no_resize;
		unsigned int  step_count = 0; // each step takes 1 ms

		unsigned int next_pool_size;

		mutex::scoped_lock lock(m_threadsMutex);

		while( m_stopPool == false )
		{
			resize_flags step_flag;

			if ( active_tasks() == pool_size() && !m_pendingTasks.empty() )
			{ // pool is full and there are pending tasks
				step_flag = flag_resize_up;
			}
			else if ( active_tasks() < pool_size() / 4 )
			{ // at least the 75% of the threads in the pool are idle
				step_flag = flag_resize_down;
			}
			else
			{ // load is greater than 25% but less than 100%, it's ok
				step_flag = flag_no_resize;
			}

			if ( step_flag != resize_flag )
			{ // changes the resize flag and resets the counter
				step_count = 0;
				resize_flag = step_flag;
			}
			else
			{ // increments the counter
				step_count += 1;

				if ( resize_flag == flag_resize_up && step_count == MAX_STEPS_UP )
				{ // max steps reached, pool size has to be increased

					next_pool_size = min(m_maxThreads, unsigned(pool_size()*RESIZE_UP_FACTOR));
					while ( pool_size() < next_pool_size )
					{
						add_thread();
					}

					resize_flag = flag_no_resize;
					step_count = 0;
				}
				else if ( resize_flag == flag_resize_down && step_count == MAX_STEPS_DOWN )
				{ // max steps reached, stop wasting resources

					next_pool_size = max(m_minThreads, unsigned(pool_size()/RESIZE_DOWN_FACTOR));

					remove_idle_threads( pool_size() - next_pool_size );

					resize_flag = flag_no_resize;
					step_count = 0;
				}
			}

			// if condition is set m_stopPool was set to true
			m_monitorCondition.timed_wait(lock, THREAD_SLEEP_MS);
		}
	}
};

pool::pool(unsigned int min_threads, unsigned int max_threads, unsigned int timeout_add_threads_ms,
           unsigned int timeout_del_threads_ms, shutdown_option on_shutdown)
 : pimpl(new impl(min_threads, max_threads, timeout_add_threads_ms, timeout_del_threads_ms, on_shutdown))
{
}

pool::pool( const pool& )
 : enable_shared_from_this<pool>()
{ // private
}

const pool& pool::operator=( const pool& )
{ // private
	return *this;
}

pool::~pool()
{
}

void pool::schedule(const task_type& task)
{
	pimpl->schedule(task);
}

void pool::schedule(const task_type& task, const boost::system_time &abs_time)
{
	pimpl->schedule(task, abs_time);
}

void pool::schedule(const task_type& task, const boost::posix_time::time_duration &rel_time)
{
	pimpl->schedule(task, get_system_time() + rel_time);
}

unsigned int pool::active_tasks()
{
	return pimpl->active_tasks();
}

unsigned int pool::pending_tasks()
{
	return pimpl->pending_tasks();
}

unsigned int pool::pool_size()
{
	return pimpl->pool_size();
}

} // namespace threadpool
