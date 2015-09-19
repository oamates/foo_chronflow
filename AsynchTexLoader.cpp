#include "stdafx.h"
#include <process.h>
#include "config.h"
#include "Helpers.h"

#include "AsynchTexLoader.h"

#include "AppInstance.h"
#include "Console.h"
#include "ScriptedCoverPositions.h"
#include "DbAlbumCollection.h"
#include "ImgTexture.h"
#include "RenderThread.h"


AsynchTexLoader::AsynchTexLoader(AppInstance* instance) :
appInstance(instance),
queueCenter(instance->albumCollection->end()),
queueCenterMoved(false),
loadingTexture(0),
noCoverTexture(0),
workerThread(0),
deleteBufferIn(0),
deleteBufferOut(0),
uploadQueueHead(0),
uploadQueueTail(0),
glRC(0),
glDC(0),
workerThreadMayRun(true, true),
mayUpload(true, true)
{
	loadSpecialTextures();
	createLoaderWindow();
	ZeroMemory(deleteBuffer,sizeof(deleteBuffer));
	startWorkerThread();
}

void AsynchTexLoader::loadSpecialTextures(){
	if (loadingTexture){
		loadingTexture->glDelete();
		delete loadingTexture;
	}
	pfc::string8 loadingTexPath(cfgImgLoading);
	loadingTexPath.skip_trailing_char(' ');
	if (loadingTexPath.get_length() > 0){
		Helpers::fixPath(loadingTexPath);
		loadingTexture = new ImgTexture(loadingTexPath);
	}
	else {
		loadingTexture = new ImgTexture(IDR_COVER_LOADING, L"JPG");
	}

	if (noCoverTexture){
		noCoverTexture->glDelete();
		delete noCoverTexture;
	}
	pfc::string8 noCoverTexPath(cfgImgNoCover);
	noCoverTexPath.skip_trailing_char(' ');
	if (noCoverTexPath.get_length() > 0){
		Helpers::fixPath(noCoverTexPath);
		noCoverTexture = new ImgTexture(noCoverTexPath);
	}
	else {
		noCoverTexture = new ImgTexture(IDR_COVER_NO_IMG, L"JPG");
	}
}

AsynchTexLoader::~AsynchTexLoader(void)
{
	IF_DEBUG(Console::println(L"Destroying AsynchTexLoader"));
	stopWorkerThread();
	if (uploadQueueHead){
		UploadQueueElem* prevE;
		UploadQueueElem* e = uploadQueueHead;
		while (e){
			prevE = e;
			e = prevE->next;
			delete prevE;
		}
	}
	// ugly, do better? - maybe window hook
	if (IsWindow(glWindow)){
		IF_DEBUG(Console::println(L" - Window still existed"));
		clearCache();
		reserveRenderContext();
		loadingTexture->glDelete();
		noCoverTexture->glDelete();
		runGlDelete();
		releaseRenderContext();
	} else {
		IF_DEBUG(Console::println(L" - Window was already gone"));
		// Has no effect, but prevents error message
		loadingTexture->glDelete();
		noCoverTexture->glDelete();
		// Do this even if we can�t delete
		for (auto it = textureCache.begin(); it != textureCache.end(); ++it){
			it->texture->glDelete();
			if (it->texture)
				delete it->texture;
		}
	}
	destroyLoaderWindow();
	delete loadingTexture;
	delete noCoverTexture;
}

void AsynchTexLoader::createLoaderWindow(){
	glWindow = CreateWindowEx( 
		0,
		L"Chronflow LoaderWindow",        // class name                   
		L"Chronflow Loader Window",       // window name                  
		WS_DISABLED | WS_CHILD,
		CW_USEDEFAULT,          // default horizontal position  
		CW_USEDEFAULT,          // default vertical position    
		CW_USEDEFAULT,          // default width                
		CW_USEDEFAULT,          // default height               
		appInstance->mainWindow,// parent or owner window    
		(HMENU) NULL,           // class menu used              
		core_api::get_my_instance(),// instance handle              
		NULL);                  // no window creation data

	if (!glWindow){
		errorPopupWin32("TexLoader failed to create a window.");
		throw new pfc::exception();
	}

	if (!(glDC = GetDC(glWindow))){
		errorPopupWin32("TexLoader failed to get a Device Context");
		destroyLoaderWindow();
		throw new pfc::exception();
	}

	int pixelFormat = appInstance->renderer->getPixelFormat();
	PIXELFORMATDESCRIPTOR pfd;
	DescribePixelFormat(glDC, pixelFormat, sizeof(PIXELFORMATDESCRIPTOR), &pfd);

	if(!SetPixelFormat(glDC,pixelFormat,&pfd)){
		errorPopupWin32("TexLoader failed to set PixelFormat");
		destroyLoaderWindow();
		throw new pfc::exception();
	}

	if (!(glRC = wglCreateContext(glDC))){
		errorPopupWin32("TexLoader failed to create a Rendering Context");
		destroyLoaderWindow();
		throw new pfc::exception();
	}

	if (!appInstance->renderer->shareLists(glRC)){
		errorPopupWin32("TexLoader failed to share Display Lists");
		destroyLoaderWindow();
		throw new pfc::exception();
	}

	wglMakeCurrent(NULL, NULL);
}



ImgTexture* AsynchTexLoader::getLoadedImgTexture(CollectionPos pos)
{
	// TODO: this needs locking?
	int idx = appInstance->albumCollection->rank(pos);

	ImgTexture* tex;
	textureCacheCS.enter();
	auto entry = textureCache.find(idx);
	if (entry != textureCache.end()){
		if (entry->texture == 0)
			tex = noCoverTexture;
		else
			tex = entry->texture;
	} else {
		tex = loadingTexture;
	}
	textureCacheCS.leave();
	return tex;
}

void AsynchTexLoader::setQueueCenter(CollectionPos center)
{
	// The new center might be from a new collection
#ifdef BOOST_MULTI_INDEX_ENABLE_SAFE_MODE
	if ((center.owner() != queueCenter.owner()) || !(center == queueCenter)){
#else
	if (!(center == queueCenter)){
#endif
		queueCenter = center;
		queueCenterMoved = true;
		workerThreadHasWork.setSignal();
	}
}

void AsynchTexLoader::setWorkerThreadPrio(int nPrio, bool force)
{
	if (workerThreadPrio != nPrio || force){
		workerThreadPrio = nPrio;
		SetThreadPriority(workerThread,nPrio);
	}
}

void AsynchTexLoader::startWorkerThread()
{
	closeWorkerThread = false;
	workerThreadMayRun.resetSignal();
	workerThread = (HANDLE)_beginthreadex(0,0,&(this->runWorkerThread),(void*)this,0,0);
	setWorkerThreadPrio(cfgTexLoaderPrio, true);
}

unsigned int WINAPI AsynchTexLoader::runWorkerThread(void* lpParameter)
{
	((AsynchTexLoader*)(lpParameter))->workerThreadProc();
	return 0;
}

void AsynchTexLoader::stopWorkerThread()
{
	if (workerThread){
		closeWorkerThread = true;
		workerThreadMayRun.setSignal();
		workerThreadHasWork.setSignal();
		WaitForSingleObject(workerThread,INFINITE);
		IF_DEBUG(Console::println(L"AsyncTexLoader worker thread stopped"));
		CloseHandle(workerThread);
		workerThread = 0;
	}
}

void AsynchTexLoader::pauseLoading()
{
	workerThreadMayRun.resetSignal();
}

void AsynchTexLoader::resumeLoading()
{
	workerThreadHasWork.setSignal();
	workerThreadMayRun.setSignal();
}

void AsynchTexLoader::destroyLoaderWindow()
{
	if (glRC){
		if (!wglDeleteContext(glRC)){
			errorPopupWin32("failed to destroy  AsyncTexLoader Render Context");
		}
		glRC = NULL;
	}

	if (glWindow)
		DestroyWindow(glWindow);
}

void AsynchTexLoader::reserveRenderContext()
{
	renderContext.enter();
	if (!wglMakeCurrent(glDC, glRC)){
		IF_DEBUG(errorPopupWin32("failed to reserve AsyncTexLoader Render Context"));
	}
}

void AsynchTexLoader::releaseRenderContext()
{
	if (!wglMakeCurrent(NULL, NULL)){
		IF_DEBUG(errorPopupWin32("failed to release AsyncTexLoader Render Context"));
	}
	renderContext.leave();
}


void AsynchTexLoader::clearCache()
{
	ScopeCS scopeLock(workerThreadInLoop);
	reserveRenderContext();
	for (auto it = textureCache.begin(); it != textureCache.end(); ++it){
		it->texture->glDelete();
		delete it->texture;
	}
	textureCache.clear();
	queueCenterMoved = true;
	releaseRenderContext();
}

void AsynchTexLoader::workerThreadProc()
{
	CollectionPos leftLoader = queueCenter;
	CollectionPos rightLoader = queueCenter;
	int leftDistance = 0;
	int rightDistance = 0;
	int loadCount = 0;
	bool nearCenter = true;
	bool onScreen = true;
	int notifyThreshold = 10;
	int notifyC = 0;
	while(!closeWorkerThread){
		workerThreadMayRun.waitForSignal();

		if (closeWorkerThread)
			break;

		if (uploadQueueHead && mayUpload.waitForSignal(0)){
			reserveRenderContext();
			doUploadQueue();
			releaseRenderContext();
		} else {
			
			// work with thread-local copy to be Thread-safe � FIXME this copying is not safe
			CollectionPos queueCenterLoc = queueCenter;
				
			if (queueCenterMoved){
				queueCenterMoved = false;
				loadCount = 0;
				leftLoader = queueCenterLoc;
				rightLoader = queueCenterLoc;
				leftDistance = 0;
				rightDistance = 0;
			}

			appInstance->albumCollection->texloaderThreadCS.enter();
			int collectionSize = appInstance->albumCollection->getCount();
			appInstance->albumCollection->texloaderThreadCS.leave();

			if ((loadCount >= int((cfgTextureCacheSize - 1)*0.8)) ||
				((loadCount + 1) >= collectionSize)){
				// Loaded enough textures
				if (uploadQueueHead){
					workerThreadHasWork.waitForTwo(mayUpload, false);
				} else {
					workerThreadHasWork.waitForSignal();
				}
			} else {
				// Load another texture
				ScopeCS scopeLock(workerThreadInLoop);
				ScopeCS scopeLockCollection(appInstance->albumCollection->texloaderThreadCS);
				CollectionPos loadNext = queueCenterLoc;
				bool foundCoverToLoad = false;
				int loadOffset = 0;
				if (loadCount == 0){
					foundCoverToLoad = true;
				} else {
					if (loadCount % 2){
						loadNext = rightLoader;
						++loadNext;
						if (loadNext != appInstance->albumCollection->end()){
							foundCoverToLoad = true;
							rightLoader = loadNext;
							rightDistance++;
							loadOffset = rightDistance;
						}
					}
					if (!foundCoverToLoad){
						if (leftLoader != appInstance->albumCollection->begin()){
							foundCoverToLoad = true;
							--leftLoader;
							loadNext = leftLoader;
							leftDistance++;
							loadOffset = -leftDistance;
						}
					}
				}
				loadCount++;
				
				// unclean / hack - this is not mulithread safe!
				onScreen = (loadOffset >= appInstance->coverPos->getFirstCover()) && (loadOffset <= appInstance->coverPos->getLastCover());

				if (loadCount < 16){
					if (!nearCenter){
						nearCenter = true;
						setWorkerThreadPrio(cfgTexLoaderPrio);
					}
				} else {
					if (nearCenter){
						nearCenter = false;
						setWorkerThreadPrio(THREAD_PRIORITY_IDLE);
					}
				}
				
				bool doUpload = false;
				if (mayUpload.waitForSignal(0)){
					doUpload = true;
				}
				reserveRenderContext();
				bool textureLoaded = loadTexImage(loadNext, doUpload);
				releaseRenderContext();
				{ // Redraw Mainwindow
					if (textureLoaded && (
						(nearCenter && (notifyC % 2)) ||
						(onScreen && ((notifyC % 4) == 0)) ||
						(loadCount == 1) ||
						(++notifyC == notifyThreshold))){
						notifyC = 0;
						notifyThreshold = loadCount * loadCount / cfgTextureCacheSize;
						IF_DEBUG(Console::println(L"                    Refresh trig."));
						appInstance->redrawMainWin();
					}
				}
			}
		}
	}
}

bool AsynchTexLoader::loadTexImage(CollectionPos pos, bool doUpload){
	int idx = appInstance->albumCollection->rank(pos);
	auto &mruIndex = textureCache.get<1>();

	textureCacheCS.enter();
	auto oldEntry = textureCache.find(idx);
	if (oldEntry != textureCache.end()){ //already loaded
		// move to front of MRU
		mruIndex.relocate(mruIndex.begin(), textureCache.project<1>(oldEntry));
		textureCacheCS.leave();
		if (oldEntry->texture && doUpload)
			oldEntry->texture->glUpload();
		return false;
	}
	textureCacheCS.leave();
	IF_DEBUG(Console::println(L"                 x"));
	ImgTexture * tex;
	tex = appInstance->albumCollection->getImgTexture(pos);
	if (doUpload && tex){
		tex->glUpload();
	} else {
		UploadQueueElem* e = new UploadQueueElem;
		e->next = 0;
		e->texIdx = idx;
		if (uploadQueueTail)
			uploadQueueTail->next = e;
		else
			uploadQueueHead = e;
		uploadQueueTail = e;
	}
	textureCacheCS.enter();
	mruIndex.push_front({ idx, tex });

	if (textureCache.size() > (size_t)cfgTextureCacheSize){
		CacheItem deleteEntry = mruIndex.back();
		mruIndex.pop_back();
		if (deleteEntry.texture)
			queueGlDelete(deleteEntry.texture);
	}
	textureCacheCS.leave();
	return true;
}

void AsynchTexLoader::allowUpload(){
	mayUpload.setSignal();
}

void AsynchTexLoader::blockUpload(){
	mayUpload.resetSignal();
}

void AsynchTexLoader::doUploadQueue(){
	UploadQueueElem* e = uploadQueueHead;
	while (e){
		if (!mayUpload.waitForSignal(0))
			break;
		auto cacheEntry = textureCache.find(e->texIdx);

		if(cacheEntry != textureCache.end() && cacheEntry->texture){
			cacheEntry->texture->glUpload();
		}
		if (e == uploadQueueTail)
			uploadQueueTail = 0;
		uploadQueueHead = e->next;
		delete e;
		e = uploadQueueHead;
	}
}

void AsynchTexLoader::queueGlDelete(ImgTexture* tex){
	if (tex == 0)
		return;
	deleteBuffer[deleteBufferIn] = tex;
	int target = deleteBufferIn + 1; // needed because of multithreading
	if (target == DELETE_BUFFER_SIZE)
		target = 0;
	if (target == deleteBufferOut)
		deleteBufferFreed.waitForSignal();
	deleteBufferIn = target;
}

void AsynchTexLoader::runGlDelete(){
	while (deleteBufferOut != deleteBufferIn){
		deleteBuffer[deleteBufferOut]->glDelete();
		delete deleteBuffer[deleteBufferOut];
		int target = deleteBufferOut + 1; // needed because of multithreading
		if (target == DELETE_BUFFER_SIZE)
			target = 0;
		deleteBufferOut = target;
	}
	deleteBufferFreed.setSignal();
}