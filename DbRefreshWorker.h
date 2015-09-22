#include <process.h>

class DbAlbumCollection::RefreshWorker {
private:
	metadb_handle_list library;
	DbAlbums albums;
	service_ptr_t<titleformat_object> albumMapper;

private:
	void moveDataToAppInstance(){
		ASSERT_APP_EXCLUSIVE(appInstance);
		appInstance->albumCollection->albums = std::move(albums);
		appInstance->albumCollection->albumMapper = std::move(albumMapper);
	}
private:
	// call from mainthread!
	void init(){
		static_api_ptr_t<library_manager> lm;
		lm->get_all_items(library);
	}

	void generateData(){
		static_api_ptr_t<titleformat_compiler> compiler;
		static_api_ptr_t<metadb> db;
		double actionStart;


		actionStart = Helpers::getHighresTimer();
		if (!cfgFilter.is_empty()){
			try {
				search_filter::ptr filter = static_api_ptr_t<search_filter_manager>()->create(cfgFilter);
				pfc::array_t<bool> mask;
				mask.set_size(library.get_count());
				filter->test_multi(library, mask.get_ptr());
				library.filter_mask(mask.get_ptr());
			}
			catch (pfc::exception const &) {};
		}
		console::printf("foo_chronflow: Filter took %d msec", int((Helpers::getHighresTimer() - actionStart) * 1000));

		auto &groupIndex = albums.get<0>();
		actionStart = Helpers::getHighresTimer();
		{
			compiler->compile(albumMapper, cfgGroup);
			pfc::string8_fast_aggressive tmpSortString;
			albums.reserve(library.get_size());
			service_ptr_t<titleformat_object> sortFormatter;
			if (!cfgSortGroup){
				compiler->compile(sortFormatter, cfgSort);
			}
			for (t_size i = 0; i < library.get_size(); i++){
				pfc::string8_fast groupString;
				metadb_handle_ptr track = library.get_item(i);
				track->format_title(0, groupString, albumMapper, 0);
				if (!groupIndex.count(groupString.get_ptr())){
					std::wstring sortString;
					if (cfgSortGroup){
						sortString = pfc::stringcvt::string_wide_from_utf8(groupString);
					} else {
						track->format_title(0, tmpSortString, sortFormatter, 0);
						sortString = pfc::stringcvt::string_wide_from_utf8(tmpSortString);
					}

					pfc::string8_fast findAsYouType;
					track->format_title(0, findAsYouType, cfgAlbumTitleScript, 0);

					metadb_handle_list tracks;
					tracks.add_item(track);
					albums.insert({ groupString.get_ptr(), std::move(sortString), std::move(findAsYouType), std::move(tracks) });
				} else {
					auto album = groupIndex.find(groupString.get_ptr());
					groupIndex.modify(album, [&](DbAlbum &a) { a.tracks.add_item(track); });
				}
			}
		}
		console::printf("foo_chronflow: Generation took %d msec", int((Helpers::getHighresTimer() - actionStart) * 1000));
	}

private:
	AppInstance* appInstance;
	RefreshWorker(AppInstance* instance){
		this->appInstance = instance;
	}
private:
	HANDLE workerThread;
	void startWorkerThread()
	{
		workerThread = (HANDLE)_beginthreadex(0, 0, &(this->runWorkerThread), (void*)this, 0, 0);
		SetThreadPriority(workerThread, THREAD_PRIORITY_BELOW_NORMAL);
		SetThreadPriorityBoost(workerThread, true);
	}

	static unsigned int WINAPI runWorkerThread(void* lpParameter)
	{
		((RefreshWorker*)(lpParameter))->workerThreadProc();
		return 0;
	}

	void workerThreadProc(){
		this->generateData();
		PostMessage(appInstance->mainWindow, WM_COLLECTION_REFRESHED, 0, (LPARAM)this);
		// Notify mainWindow to copy data, after copying, refresh AsynchTexloader + start Loading
	}

public:
	static void reloadAsynchStart(AppInstance* instance){
		instance->texLoader->pauseLoading();
		RefreshWorker* worker = new RefreshWorker(instance);
		worker->init();
		worker->startWorkerThread();
	}
	void reloadAsynchFinish(){
		ASSERT_APP_EXCLUSIVE(appInstance);
		appInstance->albumCollection->reloadSourceScripts();
		appInstance->texLoader->clearCache();

		CollectionPos newCenteredPos;
		auto &sortedIndex = albums.get<1>();

		if (appInstance->albumCollection->getCount() == 0){
			if (albums.size() > (t_size)sessionSelectedCover){
				newCenteredPos = sortedIndex.nth(sessionSelectedCover);
			} else {
				newCenteredPos = sortedIndex.begin();
			}
		} else {
			CollectionPos oldCenteredPos = appInstance->renderer->getCenteredPos();
			newCenteredPos = sortedIndex.begin();

			pfc::string8_fast_aggressive albumKey;
			for (t_size i = 0; i < oldCenteredPos->tracks.get_size(); i++){
				newCenteredPos->tracks[i]->format_title(0, albumKey, albumMapper, 0);
				if (albums.count(albumKey.get_ptr())){
					newCenteredPos = albums.project<1>(albums.find(albumKey.get_ptr()));
					break;
				}
			}
		}



		moveDataToAppInstance();
		appInstance->renderer->hardSetCenteredPos(newCenteredPos);
		appInstance->albumCollection->setTargetPos(newCenteredPos);
		appInstance->texLoader->setQueueCenter(newCenteredPos);
		// smarter target sync:
		/*if (newCenteredIdx >= 0){
		centeredPos += (newCenteredIdx - centeredIdx);
		appInstance->displayPos->hardSetCenteredPos(centeredPos);
		int newTargetIdx = resynchCallback(targetIdx, collection);
		if (newTargetIdx >= 0){
		targetPos += (newTargetIdx - targetIdx);
		} else {
		targetPos += (newCenteredIdx - centeredIdx);
		}

		}*/

		appInstance->texLoader->resumeLoading();
		appInstance->redrawMainWin();
		delete this;
	}
};