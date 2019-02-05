#pragma once
#include "TextureCache.h"
#include "DisplayPosition.h"
#include "DbAlbumCollection.h"
#include "DbReloadWorker.h"
#include "BlockingQueue.h"
#include "Renderer.h"
#include "FindAsYouType.h"
#include "PlaybackTracer.h"

class GLContext {
public:
	GLContext(class RenderWindow&);
};


class Engine {
public:
	GLContext glContext;
	DbAlbumCollection db;
	FindAsYouType findAsYouType;
	DisplayPosition displayPos;
	TextureCache texCache;
	Renderer renderer;
	PlaybackTracer playbackTracer;
	unique_ptr<DbReloadWorker> reloadWorker;

	RenderWindow& window;
	class RenderThread& thread;

	Engine(RenderThread&, RenderWindow&);
	void mainLoop();
	void onPaint();
	void updateRefreshRate();
	void onTargetChange(bool userInitiated);

private:
	bool doPaint = false;
	int timerResolution = 10;
	bool timerInPeriod = false;
	int refreshRate = 60;
	double afterLastSwap = 0;

public:
	struct Messages;
};

using EM = Engine::Messages;

#include "engine_messages.h"
