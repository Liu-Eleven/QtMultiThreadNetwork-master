#include "NetworkManager.h"
#include <QMutexLocker>
#include <QCoreApplication>
#include <QThreadPool>
#include <QThread>
#include <QMutex>
#include <QMap>
#include <QQueue>
#include <QEvent>
#include <QDebug>
#include "Log4cplusWrapper.h"
#include "NetworkRequestThread.h"


#define MAX_THREAD_COUNT 4
class NetworkManagerPrivate
{
	Q_DECLARE_PUBLIC(NetworkManager)

public:
	NetworkManagerPrivate();
	~NetworkManagerPrivate();

private:
	NetworkReply *addRequest(const QUrl& url, quint64& uiTaskId);
	NetworkReply *addBatchRequest(const RequestTasks& tasks, quint64& uiBatchId);

	bool startRequest(NetworkRequestThread *thread);
	void startNextRequest();

	void stopAllRequest();
	void stopRequest(quint64 uiTaskId);
	void stopBatchRequests(quint64 uiBatchId);

	bool killRequestThread(quint64 uiId);
	void waitForIdleThread();

	bool setMaxThreadCount(int iMax);
	int maxThreadCount() const;

	bool isRequestValid(const QUrl &url) const;
	bool isThreadAvailable() const;
	bool isWaitQueueEmpty() const;

	bool addToFailedQueue(const RequestTask &request);
	void addToWaitQueue(const RequestTask &request);
	RequestTask takeFromWaitQueue();
	void clearFailedQueue();

	NetworkReply *getReply(quint64 uiId, bool bRemove = true);
	NetworkReply *getBatchReply(quint64 uiBatchId, bool bRemove = true);
	qint64 updateBatchProgress(quint64 uiId, quint64 uiBatchId, qint64 iBytes, bool bDownload);

	quint64 newRequestId() const;
	quint64 newBatchId() const;

	void initialize();
	void reset();
	void resetData();
	void resetStopAllFlag();
	bool isStopAll() const;

private:
	Q_DISABLE_COPY(NetworkManagerPrivate);
	NetworkManager *q_ptr;

private:
	static quint64 ms_uiRequestId;
	static quint64 ms_uiBatchId;
	bool m_bStopAllFlag;	// 停止所有请求标记

	mutable QMutex m_mutex;
	QThreadPool *m_pThreadPool;

	QMap<quint64, NetworkRequestThread *> m_mapRequestThread;
	QMap<quint64, NetworkReply *> m_mapReply;			// (requestId <---> NetworkReply *)
	QMap<quint64, NetworkReply *> m_mapBatchReply;		// (batchId <---> NetworkReply *)

	QQueue<RequestTask> m_queWaiting;		// 请求等待队列
	QMap<quint64, RequestTask> m_queFailed;	// 请求失败队列

	// (batchId <---> 任务总数)
	QMap<quint64, int> m_mapBatchTotalSize;
	// (batchId <----> 任务完成数)
	QMap<quint64, int> m_mapBatchFinishedSize;

	// (<batchId, <requestId, 下载字节数>>)
	QMap<quint64, QMap<quint64, qint64>> m_mapBatchDownloadCurrentBytes;
	// (batchId <---> 总下载字节数)
	QMap<quint64, qint64> m_mapBatchDownloadTotalBytes;
	// (<batchId, <requestId, 上传字节数>>)
	QMap<quint64, QMap<quint64, qint64>> m_mapBatchUploadCurrentBytes;
	// (batchId <---> 总上传字节数)
	QMap<quint64, qint64> m_mapBatchUploadTotalBytes;
};
quint64 NetworkManagerPrivate::ms_uiRequestId = 0;
quint64 NetworkManagerPrivate::ms_uiBatchId = 0;


NetworkManagerPrivate::NetworkManagerPrivate()
	: m_mutex(QMutex::Recursive)
	, m_bStopAllFlag(false)
	, m_pThreadPool(new QThreadPool)
{
}

NetworkManagerPrivate::~NetworkManagerPrivate()
{
	LOG_FUN("");
	LOG_INFO("Reply size: " << m_mapReply.size());
	LOG_INFO("BatchReply size: " << m_mapBatchReply.size());
	LOG_INFO("RequestThread size: " << m_mapRequestThread.size());

	stopAllRequest();
	reset();
	m_pThreadPool->clear();
	m_pThreadPool->waitForDone(3000);
	m_pThreadPool->deleteLater();
}

void NetworkManagerPrivate::initialize()
{
	int nIdeal = QThread::idealThreadCount();
	if (-1 != nIdeal)
	{
		m_pThreadPool->setMaxThreadCount(nIdeal);
	}
	else
	{
		m_pThreadPool->setMaxThreadCount(MAX_THREAD_COUNT);
	}
	LOG_INFO("idealThreadCount: " << nIdeal);
	LOG_INFO("maxThreadCount: " << m_pThreadPool->maxThreadCount());

	//To add something intialize...
}

void NetworkManagerPrivate::reset()
{
	QMutexLocker locker(&m_mutex);

	m_mapRequestThread.clear();

	qDeleteAll(m_mapReply);
	m_mapReply.clear();

	qDeleteAll(m_mapBatchReply);
	m_mapBatchReply.clear();
}

void NetworkManagerPrivate::resetData()
{
	QMutexLocker locker(&m_mutex);

	m_queWaiting.clear();
	m_queFailed.clear();

	m_mapBatchTotalSize.clear();
	m_mapBatchFinishedSize.clear();
	m_mapBatchDownloadCurrentBytes.clear();
	m_mapBatchDownloadTotalBytes.clear();
	m_mapBatchUploadCurrentBytes.clear();
	m_mapBatchUploadTotalBytes.clear();
}

void NetworkManagerPrivate::resetStopAllFlag()
{
	QMutexLocker locker(&m_mutex);
	if (m_bStopAllFlag)
	{
		m_bStopAllFlag = false;
	}
}

bool NetworkManagerPrivate::isStopAll() const
{
	QMutexLocker locker(&m_mutex);
	return m_bStopAllFlag;
}

void NetworkManagerPrivate::stopAllRequest()
{
	QMutexLocker locker(&m_mutex);
	if (!m_bStopAllFlag)
	{
		m_bStopAllFlag = true;
		for (auto iter = m_mapRequestThread.begin(); iter != m_mapRequestThread.end();++iter)
		{
			NetworkRequestThread *pThread = iter.value();
			if (pThread)
			{
				pThread->quit();
			}
		}
		m_mapRequestThread.clear();

		if (!m_mapReply.isEmpty())
		{
			NetworkReply *reply = m_mapReply.last();
			if (reply)
			{
				RequestTask task;
				task.uiId = 0xFFFF;
				task.bSuccess = false;
				task.bytesContent = QString("Operation canceled (All Request)").toUtf8();

				ReplyResultEvent *event = new ReplyResultEvent;
				event->request = task;
				event->bDestroyed = true;
				QCoreApplication::postEvent(reply, event);
			}
		}

		resetData();
		reset();
	}
}

void NetworkManagerPrivate::stopRequest(quint64 uiTaskId)
{
	QMutexLocker locker(&m_mutex);
	RequestTask task;

	//qDebug() << "RequestThread size[Before]: " << m_mapRequestThread.size();
	if (m_mapRequestThread.contains(uiTaskId))
	{
		NetworkRequestThread *thread = m_mapRequestThread.take(uiTaskId);
		if (thread)
		{
			task = thread->getTask();
			thread->quit();
		}
	}
	//qDebug() << "RequestThread size[After]: " << m_mapRequestThread.size();

	//qDebug() << "m_queWaiting size[Before]: " << m_queWaiting.size();
	auto iter = m_queWaiting.begin();
	for (; iter != m_queWaiting.end();)
	{
		if (iter->uiId == uiTaskId)
		{
			iter = m_queWaiting.erase(iter);
			task = (*iter);
			break;
		}
		else
		{
			++iter;
		}
	}
	//qDebug() << "m_queWaiting size[Before]: " << m_queWaiting.size();

	if (m_queFailed.contains(uiTaskId))
	{
		m_queFailed.remove(uiTaskId);
	}

	NetworkReply *reply = m_mapReply.take(uiTaskId);
	if (reply)
	{
		task.uiId = uiTaskId;
		task.bSuccess = false;
		task.bytesContent = QString("Operation canceled (Request id: %1)").arg(uiTaskId).toUtf8();
		
		ReplyResultEvent *event = new ReplyResultEvent;
		event->request = task;
		event->bDestroyed = true;
		QCoreApplication::postEvent(reply, event);
	}
}

void NetworkManagerPrivate::stopBatchRequests(quint64 uiBatchId)
{
	QMutexLocker locker(&m_mutex);
	//qDebug() << "RequestThread size[Before]: " << m_mapRequestThread.size();
	for (auto iter = m_mapRequestThread.begin(); iter != m_mapRequestThread.end();)
	{
		NetworkRequestThread *pThread = iter.value();
		if (pThread && pThread->batchId() == uiBatchId)
		{
			pThread->quit();
			iter = m_mapRequestThread.erase(iter);
		}
		else
		{
			++iter;
		}
	}
	//qDebug() << "RequestThread size[After]: " << m_mapRequestThread.size();

	auto iter = m_queWaiting.begin();
	for (; iter != m_queWaiting.end();)
	{
		if (iter->uiBatchId == uiBatchId)
		{
			iter = m_queWaiting.erase(iter);
		}
		else
		{
			++iter;
		}
	}

	NetworkReply *reply = m_mapBatchReply.take(uiBatchId);
	if (reply)
	{
		RequestTask task;
		task.uiBatchId = uiBatchId;
		task.bSuccess = false;
		task.bytesContent = QString("Operation canceled (Batch id: %1)").arg(uiBatchId).toUtf8();
		
		ReplyResultEvent *event = new ReplyResultEvent;
		event->request = task;
		event->bDestroyed = true;
		QCoreApplication::postEvent(reply, event);
	}

	if (m_mapBatchTotalSize.contains(uiBatchId))
	{
		m_mapBatchTotalSize.remove(uiBatchId);
	}
	if (m_mapBatchFinishedSize.contains(uiBatchId))
	{
		m_mapBatchFinishedSize.remove(uiBatchId);
	}
	if (m_mapBatchDownloadCurrentBytes.contains(uiBatchId))
	{
		m_mapBatchDownloadCurrentBytes.remove(uiBatchId);
	}
	if (m_mapBatchDownloadTotalBytes.contains(uiBatchId))
	{
		m_mapBatchDownloadTotalBytes.remove(uiBatchId);
	}
	if (m_mapBatchUploadCurrentBytes.contains(uiBatchId))
	{
		m_mapBatchUploadCurrentBytes.remove(uiBatchId);
	}
	if (m_mapBatchUploadTotalBytes.contains(uiBatchId))
	{
		m_mapBatchUploadTotalBytes.remove(uiBatchId);
	}
}

NetworkReply *NetworkManagerPrivate::addRequest(const QUrl& url, quint64& uiId)
{
	if (isRequestValid(url))
	{
		uiId = newRequestId();
		NetworkReply *pReply = new NetworkReply(false);
		m_mapReply.insert(uiId, pReply);

		return pReply;
	}
	return nullptr;
}

NetworkReply *NetworkManagerPrivate::addBatchRequest(const RequestTasks& tasks, quint64& uiBatchId)
{
	uiBatchId = newBatchId();
	m_mapBatchTotalSize[uiBatchId] = tasks.size();

	NetworkReply *pReply = new NetworkReply(true);
	m_mapBatchReply.insert(uiBatchId, pReply);

	foreach (RequestTask request, tasks)
	{
		request.uiBatchId = uiBatchId;
		request.uiId = newRequestId();
		addToWaitQueue(request);
	}

	return pReply;
}

quint64 NetworkManagerPrivate::newRequestId() const
{
	QMutexLocker locker(&m_mutex);
	return ++ms_uiRequestId;
}

quint64 NetworkManagerPrivate::newBatchId() const
{
	QMutexLocker locker(&m_mutex);
	return ++ms_uiBatchId;
}

bool NetworkManagerPrivate::startRequest(NetworkRequestThread *thread)
{
	if (m_pThreadPool->tryStart(thread))
	{
		QMutexLocker locker(&m_mutex);
		m_mapRequestThread.insert(thread->requsetId(), thread);
		return true;
	}
	return false;
}

bool NetworkManagerPrivate::setMaxThreadCount(int nMax)
{
	bool bRet = false;
	if (nMax >= 1 && nMax <= 8 && m_pThreadPool)
	{
		LOG_INFO("ThreadPool maxThreadCount: " << nMax);
		qDebug() << "ThreadPool maxThreadCount: " << nMax;
		m_pThreadPool->setMaxThreadCount(nMax);
		bRet = true;
	}
	return bRet;
}

int NetworkManagerPrivate::maxThreadCount() const
{
	if (m_pThreadPool)
	{
		return m_pThreadPool->maxThreadCount();
	}
	return -1;
}

bool NetworkManagerPrivate::isThreadAvailable() const
{
	if (m_pThreadPool)
	{
		return (m_pThreadPool->activeThreadCount() < m_pThreadPool->maxThreadCount());
	}
	return false;
}

bool NetworkManagerPrivate::isRequestValid(const QUrl &url) const
{
	return (url.isValid());
}

bool NetworkManagerPrivate::isWaitQueueEmpty() const
{
	return (m_queWaiting.isEmpty());
}

NetworkReply *NetworkManagerPrivate::getReply(quint64 uiRequestId, bool bRemove)
{
	QMutexLocker locker(&m_mutex);
	if (m_mapReply.contains(uiRequestId))
	{
		if (bRemove)
		{
			return m_mapReply.take(uiRequestId);
		}
		else
		{
			return m_mapReply.value(uiRequestId);
		}
	}
	qDebug() << QString("%1 failed! Id: ").arg(__FUNCTION__) << uiRequestId;
	LOG_ERROR(__FUNCTION__ << " failed! Id: " << uiRequestId);
	return nullptr;
}

NetworkReply *NetworkManagerPrivate::getBatchReply(quint64 uiBatchId, bool bRemove)
{
	QMutexLocker locker(&m_mutex);
	if (m_mapBatchReply.contains(uiBatchId))
	{
		if (bRemove)
		{
			return m_mapBatchReply.take(uiBatchId);
		}
		else
		{
			return m_mapBatchReply.value(uiBatchId);
		}
	}
	//qDebug() << QString("%1 failed! Batch id: %2").arg(__FUNCTION__).arg(uiBatchId);
	LOG_ERROR(__FUNCTION__ << " failed! BatchId: " << uiBatchId);
	return nullptr;
}

void NetworkManagerPrivate::addToWaitQueue(const RequestTask &request)
{
	QMutexLocker locker(&m_mutex);
	m_queWaiting.enqueue(request);
}

RequestTask NetworkManagerPrivate::takeFromWaitQueue()
{
	QMutexLocker locker(&m_mutex);
	if (!m_queWaiting.isEmpty())
	{
		return m_queWaiting.dequeue();
	}
	return RequestTask();
}

bool NetworkManagerPrivate::addToFailedQueue(const RequestTask &request)
{
	QMutexLocker locker(&m_mutex);
	if (!m_queFailed.contains(request.uiId))
	{
		m_queFailed.insert(request.uiId, request);
		return true;
	}
	return false;
}

void NetworkManagerPrivate::clearFailedQueue()
{
	QMutexLocker locker(&m_mutex);
	m_queFailed.clear();
}

qint64 NetworkManagerPrivate::updateBatchProgress(quint64 uiRequestId, quint64 uiBatchId, qint64 iBytes, bool bDownload)
{
	//postEvent()过来的都在主线程，不用加锁
	//QMutexLocker locker(&m_mutex);

	//该请求任务比上次多下载/上传的字节数
	quint64 uiIncreased = 0;
	quint64 uiTotalBytes = 0;
	if (iBytes == 0)
	{
		if (bDownload)
		{
			return m_mapBatchDownloadTotalBytes[uiBatchId];
		}
		else
		{
			return m_mapBatchUploadTotalBytes[uiBatchId];
		}
	}

	if (bDownload)
	{
		QMap<quint64, qint64> mapReqId2Bytes = m_mapBatchDownloadCurrentBytes.value(uiBatchId);
		if (mapReqId2Bytes.contains(uiRequestId))
		{
			if (iBytes > mapReqId2Bytes.value(uiRequestId))
			{
				uiIncreased = iBytes - mapReqId2Bytes.value(uiRequestId);
			}
		}
		else
		{
			uiIncreased = iBytes;
		}
		mapReqId2Bytes[uiRequestId] = iBytes;
		m_mapBatchDownloadCurrentBytes[uiBatchId] = mapReqId2Bytes;

		uiTotalBytes = m_mapBatchDownloadTotalBytes.value(ms_uiBatchId) + uiIncreased;
		m_mapBatchDownloadTotalBytes[uiBatchId] = uiTotalBytes;
	}
	else
	{
		QMap<quint64, qint64> mapReqId2Bytes = m_mapBatchUploadCurrentBytes.value(uiBatchId);
		if (mapReqId2Bytes.contains(uiRequestId))
		{
			if (iBytes > mapReqId2Bytes.value(uiRequestId))
			{
				uiIncreased = iBytes - mapReqId2Bytes.value(uiRequestId);
			}
		}
		else
		{
			uiIncreased = iBytes;
		}
		mapReqId2Bytes[uiRequestId] = iBytes;
		m_mapBatchUploadCurrentBytes[uiBatchId] = mapReqId2Bytes;

		uiTotalBytes = m_mapBatchUploadTotalBytes.value(ms_uiBatchId) + uiIncreased;
		m_mapBatchUploadTotalBytes[uiBatchId] = uiTotalBytes;
	}

	return uiTotalBytes;
}

bool NetworkManagerPrivate::killRequestThread(quint64 uiRequestId)
{
	QMutexLocker locker(&m_mutex);
	if (m_mapRequestThread.contains(uiRequestId))
	{
		NetworkRequestThread *pThread = m_mapRequestThread.take(uiRequestId);
		if (pThread)
		{
			pThread->quit();
		}
		return true;
	}
	else
	{
		LOG_ERROR("NetworkManagerPrivate::killRequestThread - request not exist, id:" << uiRequestId);
	}
	return false;
}


//////////////////////////////////////////////////////////////////////////
NetworkManager *NetworkManager::ms_pInstance = nullptr;
bool NetworkManager::ms_bIntialized = false;

NetworkManager::NetworkManager(QObject *parent) : QObject(parent)
	, d_ptr(new NetworkManagerPrivate)
{
	LOG_FUN("");
	Q_D(NetworkManager);
	d->q_ptr = this;
	qDebug() << "NetworkManager Thread : " << QThread::currentThreadId();
}

NetworkManager::~NetworkManager()
{
	LOG_FUN("");
	ms_pInstance = nullptr;
}

NetworkManager* NetworkManager::globalInstance()
{
	if (!ms_pInstance)
	{
		ms_pInstance = new NetworkManager;
	}
	return ms_pInstance;
}

void NetworkManager::deleteInstance()
{
	if (ms_pInstance)
	{
		delete ms_pInstance;
		ms_pInstance = nullptr;
	}
}

bool NetworkManager::isInstantiated()
{
	return (ms_pInstance != nullptr);
}

void NetworkManager::initialize()
{
	LOG_FUN("");
	if (!ms_bIntialized)
	{
		NetworkManager::globalInstance()->init();
		ms_bIntialized = true;
	}
}

void NetworkManager::unInitialize()
{
	LOG_FUN("");
	if (ms_bIntialized)
	{
		if (isInstantiated())
		{
			NetworkManager::globalInstance()->fini();
			NetworkManager::deleteInstance();
		}
		ms_bIntialized = false;
	}
}

bool NetworkManager::isInitialized()
{
	return ms_bIntialized;
}

void NetworkManager::init()
{
	LOG_FUN("");
	Q_D(NetworkManager);
	d->initialize();
}

void NetworkManager::fini()
{
	LOG_FUN("");
	stopAllRequest();
}

void NetworkManager::stopAllRequest()
{
	LOG_FUN("");
	Q_D(NetworkManager);
	d->stopAllRequest();
}

void NetworkManager::stopRequest(quint64 uiTaskId)
{
	LOG_FUN("");
	Q_D(NetworkManager);
	d->stopRequest(uiTaskId);
}

void NetworkManager::stopBatchRequests(quint64 uiBatchId)
{
	LOG_FUN("");
	Q_D(NetworkManager);
	d->stopBatchRequests(uiBatchId);
}

NetworkReply *NetworkManager::addRequest(RequestTask& request)
{
	Q_D(NetworkManager);
	d->resetStopAllFlag();

	NetworkReply *pReply = d->addRequest(request.url, request.uiId);
	if (pReply)
	{
		if (isThreadAvailable() && isWaitingRequestEmpty())
		{
			startRequest(request);
		}
		else
		{
			d->addToWaitQueue(request);
		}
	}
	return pReply;
}

NetworkReply *NetworkManager::addBatchRequest(const RequestTasks& tasks, quint64 &uiBatchId)
{
	Q_D(NetworkManager);
	d->resetStopAllFlag();

	uiBatchId = 0;
	if (!tasks.isEmpty())
	{
		NetworkReply *pReply = d->addBatchRequest(tasks, uiBatchId);
		if (pReply && uiBatchId > 0)
		{
			if (isThreadAvailable())
			{
				startNextRequest();
			}
		}
		return pReply;
	}
	return nullptr;
}

bool NetworkManager::startRequest(const RequestTask &request)
{
	Q_D(NetworkManager);
	NetworkRequestThread *pThread = new NetworkRequestThread(request);

	qRegisterMetaType<RequestTask>("RequestTask");
	connect(pThread, SIGNAL(requestFinished(const RequestTask &)),
		this, SLOT(onRequestFinished(const RequestTask &)));

	if (!d->startRequest(pThread))
	{
		qDebug() << "ThreadPool->tryStart() failed!";
		LOG_ERROR("ThreadPool->tryStart() failed!");

		d->addToWaitQueue(request);
		pThread->deleteLater();
		return false;
	}
	return true;
}

bool NetworkManager::setMaxThreadCount(int iMax)
{
	Q_D(NetworkManager);
	return d->setMaxThreadCount(iMax);
}

int NetworkManager::maxThreadCount()
{
	Q_D(NetworkManager);
	return d->maxThreadCount();
}

bool NetworkManager::isThreadAvailable()
{
	Q_D(NetworkManager);
	return d->isThreadAvailable();
}

bool NetworkManager::isWaitingRequestEmpty()
{
	Q_D(NetworkManager);
	return d->isWaitQueueEmpty();
}

void NetworkManager::waitForIdleThread()
{
	QCoreApplication::postEvent(this, new WaitForIdleThreadEvent);
}

bool NetworkManager::event(QEvent *event)
{
	if (event->type() == NetworkEvent::WaitForIdleThread)
	{
		startNextRequest();
		return true;
	}
	else if (event->type() == NetworkEvent::NetworkProgress)
	{
		NetworkProgressEvent *evtProgress = static_cast<NetworkProgressEvent *>(event);
		if (nullptr != evtProgress)
		{
			updateProgress(evtProgress->uiId, 
				evtProgress->uiBatchId, 
				evtProgress->iBtyes, 
				evtProgress->iTotalBtyes, 
				evtProgress->bDownload);
		}
		return true;
	}

	return QObject::event(event);
}

void NetworkManager::updateProgress(quint64 uiTargetId, quint64 uiBatchId, qint64 iBytes, qint64 iTotalBytes, bool bDownload)
{
	Q_D(NetworkManager);
	if (iBytes == 0 || iTotalBytes == 0)
		return;

	if (uiBatchId == 0)
	{
		if (bDownload)
		{
			emit downloadProgress(uiTargetId, iBytes, iTotalBytes);
		}
		else
		{
			emit uploadProgress(uiTargetId, iBytes, iTotalBytes);
		}
	}
	else if (uiBatchId > 0)//批处理请求
	{
		quint64 uiBytes = d->updateBatchProgress(uiTargetId, uiBatchId, iBytes, bDownload);
		if (bDownload)
		{
			emit batchDownloadProgress(uiBatchId, uiBytes);
		}
		else
		{
			emit batchUploadProgress(uiBatchId, uiBytes);
		}
	}
}

void NetworkManager::startNextRequest()
{
	Q_D(NetworkManager);
	if (d->isStopAll())
		return;

	if (!isWaitingRequestEmpty())
	{
		if (isThreadAvailable())
		{
			const RequestTask& request = d->takeFromWaitQueue();
			if (d->isRequestValid(request.url))
			{
				startRequest(request);
			}
			else
			{
				qDebug() << "Invalid request! url: " << request.url;
				LOG_ERROR("Invalid request! url: " << request.url.path().toStdWString());
				onRequestFinished(request);//默认bSuccess=false
			}
		}
		else
		{
			waitForIdleThread();
		}
	}
	else //等待队列空，说明没有任务了，等待所有的线程退出
	{
		int nActiveThread = d->m_pThreadPool->activeThreadCount();
		if (0 == nActiveThread)
		{
			//qDebug() << QLatin1String("All tasks finished, all threads exit!");
			d->clearFailedQueue();
		}
		else if (1 == nActiveThread) //等待最后一个工作线程退出
		{
			//qDebug() << QString("Wait for thread exit! active: %1").arg(nActiveThread);
			waitForIdleThread();
		}
	}
}

bool isValidTask(const RequestTask &request)
{
	return (request.uiId > 0);
}

void NetworkManager::onRequestFinished(const RequestTask &request)
{
	Q_D(NetworkManager);
	if (d->isStopAll() || !isValidTask(request))
		return;

	bool bNotify = request.bSuccess;

	//1.处理请求失败的情况
	if (!request.bSuccess)
	{
		// 加入到失败队列，如果成功表示第一次失败，否则表示是第二次失败
		// 第一次失败的情况: 需要将任务再次加入到等待队列重新执行一遍
		// 第二次失败的情况: 需要将任务结果反馈给用户
		if (request.bTryAgainWhileFailed && d->addToFailedQueue(request))
		{
			d->addToWaitQueue(request);
		}
		else
		{
			bNotify = true;
			/*LOG_ERROR("NetworkManager::onRequestFinished -- url: " 
			<< request.url.toString().toStdWString());*/
		}
	}

	//2.通知用户结果
	if (bNotify)
	{
		NetworkReply *pReply = nullptr;
		bool bDestroyed = true;
		if (request.uiBatchId == 0)
		{
			pReply = d->getReply(request.uiId, bDestroyed);
		}
		else if (request.uiBatchId > 0)//批处理任务
		{
			if (request.bSuccess)
			{
				int sizeFinished = d->m_mapBatchFinishedSize.value(request.uiBatchId);
				sizeFinished++;
				d->m_mapBatchFinishedSize[request.uiBatchId] = sizeFinished;

				int sizeTotal = d->m_mapBatchTotalSize.value(request.uiBatchId);
				if (sizeFinished == sizeTotal) // 所有请求完成
				{
					d->m_mapBatchTotalSize.remove(request.uiBatchId);
					d->m_mapBatchFinishedSize.remove(request.uiBatchId);

					qDebug() << QStringLiteral("Batch request finished! uiBatchId：%1").arg(request.uiBatchId);
					emit batchRequestFinished(request.uiBatchId);

					pReply = d->getBatchReply(request.uiBatchId, bDestroyed);
					bDestroyed = true;
				}
				else
				{
					bDestroyed = false;
					pReply = d->getBatchReply(request.uiBatchId, bDestroyed);
				}
			}
			else//批处理任务失败
			{
				if (!request.bAbortBatchWhileOneFailed)
				{
					bDestroyed = false;
				}
				pReply = d->getBatchReply(request.uiBatchId, bDestroyed);
			}
		}

		if (nullptr != pReply)
		{
#ifdef NETWORK_REPLY_ASYN // 异步方式通知结果，多线程下载效率更高；但是要计算显示进度的情况，会导致不同步
			ReplyResultEvent *event = new ReplyResultEvent;
			event->request = request;
			event->bDestroyed = bDestroyed;
			QCoreApplication::postEvent(pReply, event);
#else
			pReply->replyResult(request, bDestroyed);
#endif
		}
	}

	//3.如果是批处理任务失败后，并且指定了bAbortBatchWhileOneFailed，就停止该批次的任务
	if (request.uiBatchId > 0 && !request.bSuccess && request.bAbortBatchWhileOneFailed)
	{
		d->stopBatchRequests(request.uiBatchId);
	}

	//4.结束任务线程
	d->killRequestThread(request.uiId);

	//5.开始下一个请求任务
	startNextRequest();
}