#include"../inc/ThrdPool.h"
#include"../inc/xx_list.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef DEBUG
#define PRT_DBG(...)											\
{																\
	printf("erro %s %s %d\n",__FILE__,__FUNCTION__,__LINE__);	\
	printf(##__VA_ARGS__);\
}
#else
#define PRT_DBG(...)
#endif

struct TaskPool;//任务池
extern struct TaskPool *TaskPoolInit();//池初始化
extern void *TaskPoolAdd(TaskPool* taskpool, void *node);//添加任务
extern void *TaskPoolDel(TaskPool* taskpool);//删除任务(应该有规则删除什么样的)
extern int TaskPoolDestroy();

struct ThreadPool;//线程池的数据结构
extern struct ThreadPool *ThreadPoolInit();//初始化
extern int setThreadPool(ThreadPool* threadpool, int maxNum, int maxIdleNum, int minIdleNum);//设置线程属性
extern int incrThreadPool(ThreadPool* threadpool);//增加线程 +1
extern int decrThreadPool(ThreadPool* threadpool);//减少线程 -1
extern int addTaskToThread(ThreadPool* threadpool, void *task);//从任务池中获得一个节点并投递给线程池
extern void threadWork(void *node);//

//-------------任务池的设计-------------

//控制线程间同步的数据结构
typedef struct _thcontrol{
	pthread_mutex_t	mutex;
	pthread_cond_t	cond;
}thcontrol;  

//任务链表的节点
typedef struct taskList{
	void*			task;				//任务节点内容，由开发人员自己定义数据结构
	struct list_node head;				//通用链表的指针
}taskNodeZ; 

//任务池
typedef struct TaskPool{
	int 			t_maxNum;			//允许的最大的任务数
	int 			t_curNum;			//当前任务数量
	void* (*func)(void*);				//个任务具体要做的操作，函数指针实现接口
	taskNodeZ		*pool;				//头指针 (上图中的head节点)
	pthread_mutex_t mutex;				//保证数据安全
	pthread_cond_t	cond_nofull;		//任务满 加任务阻塞 线程完工发信号
	pthread_cond_t	cond_noempty;		//任务空 死循环投递入线程阻塞  添加任务发信号
}TaskPool;

//-------------线程池的设计-------------

//线程节点数据结构
typedef struct threadNode{
	void* 		pthreadpool; 			//用时需要强转
	pthread_t	tid;					//该线程的ID
	int			busy;					//该线程是否空闲
	void*		task;					//该线程处理的任务接点，这里指向一个任务节点
	thcontrol	control;				//控制结构  xxxxxxx
}threadNode;

//线程链表结构
typedef struct threadList{
    threadNode*		thread;
    struct list_node head;
}threadNodeZ;

//线程池的数据结构
typedef struct ThreadPool{
	int 			t_curNum;			//当前工作线程
	int 			t_idleNum;			//当前空闲线程
	int 			t_maxNum;			//池允许最大线程
	int 			t_maxIdleNum;		//空闲线程上限
	int 			t_minIdleNum;		//空闲线程下限
	void* (*fun)(void *);  				//该线程执行的任务，这个函数指针指向任务节点的接口函数
	threadNodeZ*	pool;				//双向链表的头
	pthread_mutex_t mutex;
	pthread_cond_t 	cond_idle;
}ThreadPool;

typedef struct ThrdPool{
	TaskPool* 		taskpool;
	ThreadPool* 	threadpool;
	pthread_t 		tid_task2work;
	pthread_t 		tid_thrNumCtl;
}ThrdPool;

int active=1;

extern TaskPool* TaskPoolInit()
{
	TaskPool* 	taskpool	= (TaskPool*)malloc(sizeof(TaskPool));
	taskpool->pool			= (taskNodeZ*)malloc(sizeof(taskNodeZ));
	taskpool->pool->task 	= NULL;
	pthread_mutex_init(&taskpool->mutex,NULL);
	pthread_cond_init(&taskpool->cond_nofull, NULL);
	pthread_cond_init(&taskpool->cond_noempty, NULL);
	INIT_list_HEAD(&taskpool->pool->head);
	taskpool->t_curNum = 0;
	return taskpool;
}

void *TaskPoolAdd(TaskPool* taskpool, void *tasknode)			//添加任务节点到 任务池
{
	if(NULL==tasknode){
		PRT_DBG("%s %d TaskPoolAdd tasknode=NULL\n",__FILE__,__LINE__);
		return (void*)NULL;
	}
	
	pthread_mutex_lock(&(taskpool->mutex));						//互斥同步,保证操作的唯一性
	while(taskpool->t_curNum >= taskpool->t_maxNum){
		PRT_DBG("taskpool->t_curNum %d task is full\n",taskpool->t_curNum);
		pthread_cond_wait(&(taskpool->cond_nofull), &(taskpool->mutex));
	}
	taskNodeZ* tmp_tasklist	= (taskNodeZ*)malloc(sizeof(taskNodeZ));
	tmp_tasklist->task		= tasknode;
	list_add(&tmp_tasklist->head,&taskpool->pool->head); 		//添加到头节点之后
	taskpool->t_curNum++;
	pthread_mutex_unlock(&(taskpool->mutex));					//解锁
	
	pthread_cond_signal(&taskpool->cond_noempty);
	return tasknode;
}

void *TaskPoolDel(TaskPool* taskpool)							//删除任务(甩出 task)//del from list ...(queue)
{
	pthread_mutex_lock(&(taskpool->mutex));
	while (taskpool->t_curNum <= 0){							/* while避免虚假唤醒*/
		PRT_DBG("%s %d %d task is empty\n",__FILE__,__LINE__,taskpool->t_curNum);
		pthread_cond_wait(&(taskpool->cond_noempty), &(taskpool->mutex));
	}
	size_t 				offset	=((size_t) &((taskNodeZ*)0)->head);
	struct list_node* 	Pos 	= taskpool->pool->head.prev;	//链表尾部取值
	taskNodeZ* 	temp_tasklist 	= (taskNodeZ*)((size_t)(char*)Pos-offset);
	void* 				task 	= (temp_tasklist->task);
	
	list_del(taskpool->pool->head.prev);
	free(temp_tasklist);
	taskpool->t_curNum--;
	pthread_mutex_unlock(&(taskpool->mutex));
	return task;
}

//-------------------------------------------------
extern ThreadPool* ThreadPoolInit()
{
	ThreadPool* 	threadpool	= (ThreadPool*)malloc(sizeof(ThreadPool));
	threadpool->pool			= (threadNodeZ*)malloc(sizeof(threadNodeZ));
	pthread_mutex_init(&threadpool->mutex,NULL);
	pthread_cond_init(&threadpool->cond_idle,NULL);
	INIT_list_HEAD(&threadpool->pool->head);						//INIT_list_HEAD(threadpool->pool);
	return threadpool;
}


extern int setThreadPool(ThreadPool* threadpool, int maxNum, int maxIdleNum, int minIdleNum)
{
	threadpool->t_curNum		= 0;
	threadpool->t_maxNum		= maxNum;
	threadpool->t_idleNum		= 0;
	threadpool->t_maxIdleNum	= maxIdleNum;
	threadpool->t_minIdleNum	= minIdleNum;
	return 0;
}


extern int incrThreadPool(ThreadPool* threadpool)
{
	pthread_mutex_lock(&(threadpool->mutex));					//互斥同步,保证操作的唯一性池
	if(threadpool->t_curNum >= threadpool->t_maxNum){
		pthread_mutex_unlock(&(threadpool->mutex));				//线程已经达到最大
		return 2;
	}
	threadNodeZ* new_threadlist	= (threadNodeZ*)malloc(sizeof(threadNodeZ));//线程链表节点
	threadNode* new_threadnode	= (threadNode*)malloc(sizeof(threadNode));
	new_threadnode->busy		= 0;
	new_threadnode->task		= NULL;
	new_threadnode->pthreadpool	= threadpool;
	new_threadlist->thread 		= new_threadnode;
	pthread_mutex_init(&new_threadnode->control.mutex,NULL);
	pthread_cond_init(&new_threadnode->control.cond,NULL);
	//
	int err = pthread_create(&(new_threadlist->thread->tid), NULL, (pfun)threadWork, new_threadnode);//创建线程线程工作内容
	if (err != 0)
		printf("can't create thread: %s\n", strerror(err));
	new_threadlist->thread->busy	=0;
	list_add(&new_threadlist->head,&threadpool->pool->head);			//调用通用链表的函数实现添加
	threadpool->t_curNum++;
	threadpool->t_idleNum++; 
	pthread_mutex_unlock(&(threadpool->mutex));						//解锁
	pthread_cond_signal(&threadpool->cond_idle);
	
	PRT_DBG("thr++%x minIdle:%d cur:%d idle:%d maxIdle:%d\n",new_threadlist->thread->tid,
	threadpool->t_minIdleNum,threadpool->t_curNum,
	threadpool->t_idleNum,threadpool->t_maxIdleNum);
	return 0;
}

extern int decrThreadPool(ThreadPool* threadpool)
{
	pthread_mutex_lock(&(threadpool->mutex));
	if(threadpool->t_curNum <= 0){
		pthread_mutex_unlock(&(threadpool->mutex));
		return 2;
	}
	threadNodeZ* tmp_threadlist = NULL;//用于暂存 闲置线程 的节点信息
	size_t offset = ((size_t) &((threadNodeZ*)0)->head);
	
	//找到一个不忙的线程
	struct list_node* Pos = threadpool->pool->head.prev;//线程节点 的指针
	do{
		tmp_threadlist = (threadNodeZ*)((size_t)(char*)Pos-offset);
		Pos = (Pos->prev);
	} while (tmp_threadlist->thread->busy);
	
	int err = pthread_cancel(tmp_threadlist->thread->tid);
	if (err != 0)
		printf("can't decrease thread: %s\n", strerror(err));
	pthread_join(tmp_threadlist->thread->tid, (void**)NULL);
	
	list_del(&tmp_threadlist->head);
	free(tmp_threadlist->thread);
	free(tmp_threadlist);
	threadpool->t_curNum--;
	threadpool->t_idleNum--; 
	pthread_mutex_unlock(&(threadpool->mutex));
	
	PRT_DBG("thr--minIdle:%d cur:%d idle:%d maxIdle:%d\n",
		threadpool->t_minIdleNum,threadpool->t_curNum,threadpool->t_idleNum,threadpool->t_maxIdleNum);
	return 0;
}

//----------------功能组件逻辑流程-------------------

//任务池容量    线程池容量    空闲上限    空闲下限    任务函数
ThrdPool* ThrdPool_init(int taskpoolnum, int maxNum, int maxIdleNum, int minIdleNum, void* (*func)(void*))
{
	ThrdPool* thrdpool		= (ThrdPool*)malloc(sizeof(ThrdPool));
	thrdpool->taskpool		= TaskPoolInit();
	thrdpool->threadpool	= ThreadPoolInit();
	
	thrdpool->taskpool->t_maxNum	= taskpoolnum;
	thrdpool->tid_task2work			= 0;
	thrdpool->tid_thrNumCtl			= 0;
	thrdpool->threadpool->fun		= func;
	setThreadPool(thrdpool->threadpool, maxNum, maxIdleNum, minIdleNum);

	return thrdpool;
}
//--------------------任务入task接口-------------------

int ThrdPool_addTask(ThrdPool* thrdpool, void *task)//
{
	TaskPoolAdd(thrdpool->taskpool, task);
}

//---------------------------启动添加线程--------------------
//说明:开工线程从任务池中取一个任务投递给线程池中的一个空线程（任务切换调节管理线程）
	int addTaskToThread(ThreadPool* threadpool, void *task);
	void loop_task2work(void* thrdpool);
pthread_t ThrdPool_run(ThrdPool* thrdpool)
{
	pthread_t tid;
	pthread_create(&tid, NULL, (pfun)loop_task2work, (void*)thrdpool);
	thrdpool->tid_task2work = tid;
	return tid;
}
//把任务节点从任务池投递到线程池，除非主管理线程发出Cancel结束信号。
void loop_task2work(void* thrdpool)
{
	int oldtype;
	while(1) {//死循添加
		pthread_testcancel();
		pthread_setcanceltype(PTHREAD_CANCEL_DEFERRED, &oldtype);
		
		TaskPool* 		taskpool	= ((ThrdPool*)thrdpool)->taskpool;
		ThreadPool* 	threadpool	= ((ThrdPool*)thrdpool)->threadpool;
		pthread_mutex_lock(&(threadpool->mutex));			//这锁目的
		while(threadpool->t_idleNum <= 0)					//无空闲线程  阻塞等
			pthread_cond_wait(&(threadpool->cond_idle),&(threadpool->mutex));
		pthread_mutex_unlock(&(threadpool->mutex));
		void *task 		= (void*)TaskPoolDel(taskpool);			//删除任务  把task甩出来
		pthread_cond_signal(&taskpool->cond_nofull);
		addTaskToThread(threadpool, task);					//任务节点投递到线程池具体工作在下面
	
		pthread_setcanceltype(oldtype, NULL);
		pthread_testcancel();
	}
}
//先说明：线程池中的线程是否空闲主要由两个参数确定：busy是否为0，task是否为NULL。
//如果是空闲的，该子线程通过cond信号阻塞，知道本管理线程发信号通知它开始工作。
int addTaskToThread(ThreadPool* threadpool, void *task)//任务节点投递到线程池的细节：
{
	if(threadpool==NULL || list_empty(&threadpool->pool->head)) 
		return -1;
	struct list_node* Pos = threadpool->pool->head.next;				//Pos 定位下一个空闲线程节点的指针位置
	while(Pos){
		size_t        offset         = ((size_t) &((threadNodeZ*)0)->head);
		threadNodeZ* threadnodez     = (threadNodeZ*)((size_t)(char*)Pos-offset);
		threadNode* threadnode       = threadnodez->thread;
		if(threadnode->busy==0 && threadnode->task==(void*)NULL){

			pthread_mutex_lock(&(threadnode->control.mutex));		//是单个线程的状态改变都是原子操作
			threadnode->busy = 1;
			threadnode->task = (void*)task;
			pthread_mutex_unlock(&(threadnode->control.mutex));
			
			pthread_mutex_lock(&(threadpool->mutex));
			threadpool->t_idleNum--; 
			pthread_mutex_unlock(&(threadpool->mutex));
			
			pthread_cond_signal(&threadnode->control.cond);			//发信号给该子线程 有任务做 停止阻塞
			Pos = Pos->next;
			
			if (threadpool->t_idleNum < threadpool->t_minIdleNum)	//小于最少空闲数量，则创建新的线程
				incrThreadPool( threadpool );						//每次使用一个空闲线程后检查当前空闲线程的数量
			break;
		}
		Pos = Pos->next;
	}
	return 0;
}

//----------------子线程的工作内容-------------------

void threadWork( void *node)
{
	int oldtype;
	if(node==NULL)
		return;
	threadNode* threadnode = (threadNode*)node;
	ThreadPool* threadpool = (ThreadPool*)threadnode->pthreadpool;
	
	while(active){
		pthread_testcancel();									//取消点 是否Canceld状态 若是则进行取消动作，否则直接返回//线程在工作期间不被中断
		pthread_setcanceltype(PTHREAD_CANCEL_DEFERRED, &oldtype);//设置cancel时，继续执行到下一个取消点 保存原有反应状态
		//pthread_cleanup_push(pthread_mutex_unlock, (void *) &(tmp_control.mutex));//防止死锁11  具体处理函数可自定义 参数也一样
		pthread_mutex_lock(&(threadnode->control.mutex));
	
		while(threadnode->busy==0 && threadnode->task==(void*)NULL){//等待任务的到来，否则堵塞在这里     非忙状态并且任务空
			// printf("---%dthread waiting here\n",threadnode->tid);
			pthread_cond_wait(&(threadnode->control.cond),&(threadnode->control.mutex));
			//printf("---%dthread actived here\n",pthread_self());
		}
		threadpool->fun((void*)threadnode->task);//真正工作 用户接口定义的 //func指向用户接口定义的函数
		threadnode->task = (void*)NULL;
		threadnode->busy = 0;
		pthread_mutex_unlock(&(threadnode->control.mutex));
		pthread_mutex_lock(&(threadpool->mutex));
		threadpool->t_idleNum++;
		pthread_mutex_unlock(&(threadpool->mutex));
		//pthread_cleanup_pop(0);				//11
		pthread_setcanceltype(oldtype, NULL);	//恢复原来的 对cancel的反应状态
		pthread_testcancel();					//取消点
		pthread_cond_signal(&threadpool->cond_idle);//通知管理投递任务到线程池中的线程，现在有了一个空闲线程可以工作
	}
}

//----------------管理线程池中子线程的线程工作情况--------------------

	void* loop_NumCtrl(void* thrdpool);
pthread_t ThrdPool_NumCtl(ThrdPool* thrdpool)//线程管理（主要是控制合理的数量）
{
	pthread_t 		tid;
	pthread_create(&tid, NULL, loop_NumCtrl, (void*)thrdpool);
	thrdpool->tid_thrNumCtl = tid;
	return tid;
}

void* loop_NumCtrl(void* thrdpool)
{
	TaskPool*		taskpool 	= ((ThrdPool*)thrdpool)->taskpool;
	ThreadPool*		threadpool 	= ((ThrdPool*)thrdpool)->threadpool;
	
	int oldtype,flag	=0;
	while(1){ 
		sleep(1);
		pthread_testcancel();
		pthread_setcanceltype(PTHREAD_CANCEL_DEFERRED, &oldtype);
		
		if(threadpool->t_curNum < threadpool->t_minIdleNum)
		incrThreadPool(threadpool);
		
		if(threadpool->t_idleNum > threadpool->t_maxIdleNum){
			if (++flag == 3)					//连续3次 空闲线程太多，就销毁一个空闲线程  过少就创建
			{
				decrThreadPool(threadpool);		//减少空闲线程
				flag = 0;
			}
		}else
			flag=0;
		pthread_setcanceltype(oldtype, NULL);
		pthread_testcancel();
	}
}

int destroyThreadPool(ThrdPool* thrdpool)
{
	TaskPool* 		taskpool	= ((ThrdPool*)thrdpool)->taskpool;
	ThreadPool* 	threadpool	= ((ThrdPool*)thrdpool)->threadpool;
	
	pthread_mutex_lock(&(threadpool->mutex));
	while (taskpool->t_curNum)//等task 全部完成
		usleep(10);
	
	threadNodeZ* 	tmp_threadlist 	= NULL;
	size_t 			offset 			= ((size_t)&((threadNodeZ*)0)->head);
	while (threadpool->t_curNum)//动态增长线程已经销毁，t_curNum不会死锁
	{
		struct list_node* Pos 	= threadpool->pool->head.prev;
		tmp_threadlist 			= (threadNodeZ*)((size_t)(char*)Pos - offset);
		Pos 					= (Pos->prev);
		int err = pthread_cancel(tmp_threadlist->thread->tid);
		if (err != 0)
			printf("can't decrease thread: %s\n", strerror(err));
		pthread_join(tmp_threadlist->thread->tid, (void**)NULL);
		
		list_del(&tmp_threadlist->head);
		free(tmp_threadlist->thread);
		free(tmp_threadlist);
		threadpool->t_curNum--;
		threadpool->t_idleNum--;
	}
	pthread_mutex_unlock(&(threadpool->mutex));
	
	PRT_DBG("------minIdle:%d cur:%d idle:%d maxIdle:%d\n",
		threadpool->t_minIdleNum, threadpool->t_curNum, threadpool->t_idleNum, threadpool->t_maxIdleNum);
}


void ThrdPool_destroy( ThrdPool* thrdpool )
{
	TaskPool*	taskpool	= ((ThrdPool*)thrdpool)->taskpool;
	ThreadPool* threadpool	= ((ThrdPool*)thrdpool)->threadpool;
	
	pthread_cancel(thrdpool->tid_thrNumCtl);			//数量控制线程销毁
	pthread_join(thrdpool->tid_thrNumCtl, (void**)NULL);
	
	destroyThreadPool(thrdpool);
	
	pthread_cancel(thrdpool->tid_task2work); 			//任务添加线程退出
	pthread_join(thrdpool->tid_task2work, (void**)NULL);
	
	//释放容器内存
	if (taskpool != NULL)
	{
		free(taskpool->pool);
		free(taskpool);
		taskpool = NULL;
	}
	if (threadpool != NULL)
	{
		free(threadpool->pool);
		free(threadpool);
		taskpool = NULL;
	}
	if (thrdpool != NULL)
		free(thrdpool);
}
