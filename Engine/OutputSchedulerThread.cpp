/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of Natron <http://www.natron.fr/>,
 * Copyright (C) 2016 INRIA and Alexandre Gauthier-Foichat
 *
 * Natron is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * Natron is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Natron.  If not, see <http://www.gnu.org/licenses/gpl-2.0.html>
 * ***** END LICENSE BLOCK ***** */

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "OutputSchedulerThread.h"

#include <iostream>
#include <set>
#include <list>
#include <algorithm> // min, max
#include <cassert>
#include <stdexcept>

#include <boost/scoped_ptr.hpp>
#include <boost/algorithm/clamp.hpp>

#include <QtCore/QMetaType>
#include <QtCore/QMutex>
#include <QtCore/QWaitCondition>
#include <QtCore/QCoreApplication>
#include <QtCore/QString>
#include <QtCore/QThreadPool>
#include <QtCore/QDebug>
#include <QtCore/QTextStream>
#include <QtCore/QRunnable>

#include "Global/MemoryInfo.h"

#include "Engine/AppManager.h"
#include "Engine/AppInstance.h"
#include "Engine/EffectInstance.h"
#include "Engine/Image.h"
#include "Engine/KnobFile.h"
#include "Engine/Node.h"
#include "Engine/KnobItemsTable.h"
#include "Engine/OpenGLViewerI.h"
#include "Engine/GenericSchedulerThreadWatcher.h"
#include "Engine/Project.h"
#include "Engine/RenderStats.h"
#include "Engine/Settings.h"
#include "Engine/Timer.h"
#include "Engine/TimeLine.h"
#include "Engine/TreeRender.h"
#include "Engine/TLSHolder.h"
#include "Engine/RotoPaint.h"
#include "Engine/RotoStrokeItem.h"
#include "Engine/UpdateViewerParams.h"
#include "Engine/ViewIdx.h"
#include "Engine/ViewerInstance.h"
#include "Engine/WriteNode.h"

#ifdef DEBUG
//#define TRACE_SCHEDULER
//#define TRACE_CURRENT_FRAME_SCHEDULER
#endif

#define NATRON_FPS_REFRESH_RATE_SECONDS 1.5

/*
   When defined, parallel frame renders are spawned from a timer so that the frames
   appear to be rendered all at the same speed.
   When undefined each time a frame is computed a new thread will be spawned
   until we reach the maximum allowed parallel frame renders.
 */
//#define NATRON_SCHEDULER_SPAWN_THREADS_WITH_TIMER

#ifdef NATRON_SCHEDULER_SPAWN_THREADS_WITH_TIMER
#define NATRON_SCHEDULER_THREADS_SPAWN_DEFAULT_TIMEOUT_MS 500
#endif

#define NATRON_SCHEDULER_ABORT_AFTER_X_UNSUCCESSFUL_ITERATIONS 5000

NATRON_NAMESPACE_ENTER;


///Sort the frames by time and then by view

struct BufferedFrameKey
{
    TimeValue time;
};

struct BufferedFrameCompare_less
{
    bool operator()(const BufferedFrameKey& lhs,
                    const BufferedFrameKey& rhs) const
    {
        return lhs.time < rhs.time;
    }
};

typedef std::multimap< BufferedFrameKey, BufferedFrame, BufferedFrameCompare_less > FrameBuffer;


NATRON_NAMESPACE_ANONYMOUS_ENTER

class MetaTypesRegistration
{
public:
    inline MetaTypesRegistration()
    {
        qRegisterMetaType<BufferedFrames>("BufferedFrames");
        qRegisterMetaType<BufferableObjectList>("BufferableObjectList");
    }
};

NATRON_NAMESPACE_ANONYMOUS_EXIT


static MetaTypesRegistration registration;
struct RenderThread
{
    RenderThreadTask* thread;
    bool active;
};

typedef std::list<RenderThread> RenderThreads;


struct ProducedFrame
{
    BufferableObjectList frames;
    U64 age;
    RenderStatsPtr stats;
};

struct ProducedFrameCompareAgeLess
{
    bool operator() (const ProducedFrame& lhs,
                     const ProducedFrame& rhs) const
    {
        return lhs.age < rhs.age;
    }
};

typedef std::set<ProducedFrame, ProducedFrameCompareAgeLess> ProducedFrameSet;

class OutputSchedulerThreadExecMTArgs
    : public GenericThreadExecOnMainThreadArgs
{
public:

    BufferedFrames frames;

    OutputSchedulerThreadExecMTArgs()
        : GenericThreadExecOnMainThreadArgs()
    {}

    virtual ~OutputSchedulerThreadExecMTArgs() {}
};

#ifndef NATRON_PLAYBACK_USES_THREAD_POOL
static bool
isBufferFull(int nbBufferedElement,
             int hardwardIdealThreadCount)
{
    return nbBufferedElement >= hardwardIdealThreadCount * 3;
}

#endif

struct OutputSchedulerThreadPrivate
{
    FrameBuffer buf; //the frames rendered by the worker threads that needs to be rendered in order by the output device
    QWaitCondition bufEmptyCondition;
    mutable QMutex bufMutex;

    //doesn't need any protection since it never changes and is set in the constructor
    OutputSchedulerThread::ProcessFrameModeEnum mode; //is the frame to be processed on the main-thread (i.e OpenGL rendering) or on the scheduler thread
    boost::scoped_ptr<Timer> timer; // Timer regulating the engine execution. It is controlled by the GUI and MT-safe.
    boost::scoped_ptr<TimeLapse> renderTimer; // Timer used to report stats when rendering

    ///When the render threads are not using the appendToBuffer API, the scheduler has no way to know the rendering is finished
    ///but to count the number of frames rendered via notifyFrameRended which is called by the render thread.
    mutable QMutex renderFinishedMutex;
    U64 nFramesRendered;
    bool renderFinished; //< set to true when nFramesRendered = runArgs->lastFrame - runArgs->firstFrame + 1

    // Pointer to the args used in threadLoopOnce(), only usable from the scheduler thread
    boost::weak_ptr<OutputSchedulerThreadStartArgs> runArgs;

    mutable QMutex lastRunArgsMutex;
    std::vector<ViewIdx> lastPlaybackViewsToRender;
    RenderDirectionEnum lastPlaybackRenderDirection;

    ///Worker threads
    mutable QMutex renderThreadsMutex;
    RenderThreads renderThreads;
    QWaitCondition allRenderThreadsInactiveCond; // wait condition to make sure all render threads are asleep


    // Protects lastFrameRequested & expectedFrameToRender & schedulerRenderDirection
    QMutex lastFrameRequestedMutex;

    // The last frame requested to render
    TimeValue lastFrameRequested;

    // The frame expected by the scheduler thread to be rendered
    TimeValue expectedFrameToRender;

    // The direction of the scheduler
    RenderDirectionEnum schedulerRenderDirection;


    boost::weak_ptr<OutputEffectInstance> outputEffect; //< The effect used as output device

    RenderEngine* engine;

    QMutex bufferedOutputMutex;
    int lastBufferedOutputSize;


    OutputSchedulerThreadPrivate(RenderEngine* engine,
                                 const OutputEffectInstancePtr& effect,
                                 OutputSchedulerThread::ProcessFrameModeEnum mode)
        : buf()
        , bufEmptyCondition()
        , bufMutex()
        , mode(mode)
        , timer(new Timer)
        , renderTimer()
        , renderFinishedMutex()
        , nFramesRendered(0)
        , renderFinished(false)
        , runArgs()
        , lastRunArgsMutex()
        , lastPlaybackViewsToRender()
        , lastPlaybackRenderDirection(eRenderDirectionForward)
        , renderThreadsMutex()
        , renderThreads()
        , allRenderThreadsInactiveCond()
        , lastFrameRequestedMutex()
        , lastFrameRequested(0)
        , expectedFrameToRender(0)
        , schedulerRenderDirection(eRenderDirectionForward)
        , outputEffect(effect)
        , engine(engine)
        , bufferedOutputMutex()
        , lastBufferedOutputSize(0)
    {
    }

    void appendBufferedFrame(TimeValue time,
                             ViewIdx view,
                             const RenderStatsPtr& stats,
                             const BufferableObjectPtr& image)
    {
        ///Private, shouldn't lock
        assert( !bufMutex.tryLock() );
#ifdef TRACE_SCHEDULER
        QString idStr;
        if (image) {
            idStr = QString::fromUtf8("ID: ") + QString::number( image->getUniqueID() );
        }
        qDebug() << "Parallel Render Thread: Rendered Frame:" << time << " View:" << (int)view << idStr;
#endif
        BufferedFrameKey key;
        BufferedFrame value;
        value.time = key.time = time;
        value.view = view;
        value.frame = image;
        value.stats = stats;
        buf.insert( std::make_pair(key, value) );
    }

    struct ViewUniqueIDPair
    {
        ViewIdx view;
        int uniqueId;
    };

    struct ViewUniqueIDPairCompareLess
    {
        bool operator() (const ViewUniqueIDPair& lhs,
                         const ViewUniqueIDPair& rhs) const
        {
            if (lhs.view < rhs.view) {
                return true;
            } else if (lhs.view > rhs.view) {
                return false;
            } else {
                if (lhs.uniqueId < rhs.uniqueId) {
                    return true;
                } else if (lhs.uniqueId > rhs.uniqueId) {
                    return false;
                } else {
                    return false;
                }
            }
        }
    };

    typedef std::set<ViewUniqueIDPair, ViewUniqueIDPairCompareLess> ViewUniqueIDSet;

    void getFromBufferAndErase(TimeValue time,
                               BufferedFrames& frames)
    {
        ///Private, shouldn't lock
        assert( !bufMutex.tryLock() );

        /*
           Note that the frame buffer does not hold any particular ordering and just contains all the frames as they
           were received by render threads.
           In the buffer, for any particular given time there can be:
           - Multiple views
           - Multiple "unique ID" (corresponds to viewer input A or B)

           Also since we are rendering ahead, we can have a buffered frame at time 23,
           and also another frame at time 23, each of which could have multiple unique IDs and so on

           To retrieve what we need to render, we extract at least one view and unique ID for this particular time
         */

        ViewUniqueIDSet uniqueIdsRetrieved;
        BufferedFrameKey key;
        key.time = time;
        std::pair<FrameBuffer::iterator, FrameBuffer::iterator> range = buf.equal_range(key);
        std::list<std::pair<BufferedFrameKey, BufferedFrame> > toKeep;
        for (FrameBuffer::iterator it = range.first; it != range.second; ++it) {
            bool keepInBuf = true;
            if (it->second.frame) {
                ViewUniqueIDPair p;
                p.view = it->second.view;
                p.uniqueId = it->second.frame->getUniqueID();
                std::pair<ViewUniqueIDSet::iterator, bool> alreadyRetrievedIndex = uniqueIdsRetrieved.insert(p);
                if (alreadyRetrievedIndex.second) {
                    frames.push_back(it->second);
                    keepInBuf = false;
                }
            }


            if (keepInBuf) {
                toKeep.push_back(*it);
            }
        }
        if ( range.first != buf.end() ) {
            buf.erase(range.first, range.second);
            buf.insert( toKeep.begin(), toKeep.end() );
        }
    } // getFromBufferAndErase

    void startRunnable(RenderThreadTask* runnable)
    {
        assert( !renderThreadsMutex.tryLock() );
        RenderThread r;
        r.thread = runnable;
        r.active = true;
        renderThreads.push_back(r);
        QThreadPool::globalInstance()->start(runnable);
    }

    RenderThreads::iterator getRunnableIterator(RenderThreadTask* runnable)
    {
        // Private shouldn't lock
        assert( !renderThreadsMutex.tryLock() );
        for (RenderThreads::iterator it = renderThreads.begin(); it != renderThreads.end(); ++it) {
            if (it->thread == runnable) {
                return it;
            }
        }

        return renderThreads.end();
    }

    int getNBufferedFrames() const
    {
        QMutexLocker l(&bufMutex);

        return buf.size();
    }

    static bool getNextFrameInSequence(PlaybackModeEnum pMode,
                                       RenderDirectionEnum direction,
                                       TimeValue frame,
                                       TimeValue firstFrame,
                                       TimeValue lastFrame,
                                       TimeValue frameStep,
                                       TimeValue* nextFrame,
                                       RenderDirectionEnum* newDirection);
    static void getNearestInSequence(RenderDirectionEnum direction,
                                     TimeValue frame,
                                     TimeValue firstFrame,
                                     TimeValue lastFrame,
                                     TimeValue* nextFrame);


    void waitForRenderThreadsToQuitInternal()
    {
        assert( !renderThreadsMutex.tryLock() );
        while (renderThreads.size() > 0) {
            allRenderThreadsInactiveCond.wait(&renderThreadsMutex, 200);
        }
    }

    int getNActiveRenderThreads() const
    {
        // Private shouldn't lock
        assert( !renderThreadsMutex.tryLock() );
        return (int)renderThreads.size();
    }


    void waitForRenderThreadsToQuit()
    {
        QMutexLocker l(&renderThreadsMutex);
        waitForRenderThreadsToQuitInternal();
    }
};

OutputSchedulerThread::OutputSchedulerThread(RenderEngine* engine,
                                             const OutputEffectInstancePtr& effect,
                                             ProcessFrameModeEnum mode)
    : GenericSchedulerThread()
    , _imp( new OutputSchedulerThreadPrivate(engine, effect, mode) )
{
    QObject::connect( _imp->timer.get(), SIGNAL(fpsChanged(double,double)), _imp->engine, SIGNAL(fpsChanged(double,double)) );


#ifdef NATRON_SCHEDULER_SPAWN_THREADS_WITH_TIMER
    QObject::connect( &_imp->threadSpawnsTimer, SIGNAL(timeout()), this, SLOT(onThreadSpawnsTimerTriggered()) );
#endif

    setThreadName("Scheduler thread");
}

OutputSchedulerThread::~OutputSchedulerThread()
{

    // Make sure all tasks are finished, there will be a deadlock here if that's not the case.
    _imp->waitForRenderThreadsToQuit();
}

bool
OutputSchedulerThreadPrivate::getNextFrameInSequence(PlaybackModeEnum pMode,
                                                     RenderDirectionEnum direction,
                                                     TimeValue frame,
                                                     TimeValue firstFrame,
                                                     TimeValue lastFrame,
                                                     TimeValue frameStep,
                                                     TimeValue* nextFrame,
                                                     RenderDirectionEnum* newDirection)
{
    assert(frameStep >= 1);
    *newDirection = direction;
    if (firstFrame == lastFrame) {
        *nextFrame = firstFrame;

        return true;
    }
    if (frame <= firstFrame) {
        switch (pMode) {
        case ePlaybackModeLoop:
            if (direction == eRenderDirectionForward) {
                *nextFrame = TimeValue(firstFrame + frameStep);
            } else {
                *nextFrame  = TimeValue(lastFrame - frameStep);
            }
            break;
        case ePlaybackModeBounce:
            if (direction == eRenderDirectionForward) {
                *newDirection = eRenderDirectionBackward;
                *nextFrame  = TimeValue(lastFrame - frameStep);
            } else {
                *newDirection = eRenderDirectionForward;
                *nextFrame  = TimeValue(firstFrame + frameStep);
            }
            break;
        case ePlaybackModeOnce:
        default:
            if (direction == eRenderDirectionForward) {
                *nextFrame = TimeValue(firstFrame + frameStep);
                break;
            } else {
                return false;
            }
        }
    } else if (frame >= lastFrame) {
        switch (pMode) {
        case ePlaybackModeLoop:
            if (direction == eRenderDirectionForward) {
                *nextFrame = firstFrame;
            } else {
                *nextFrame = TimeValue(lastFrame - frameStep);
            }
            break;
        case ePlaybackModeBounce:
            if (direction == eRenderDirectionForward) {
                *newDirection = eRenderDirectionBackward;
                *nextFrame = TimeValue(lastFrame - frameStep);
            } else {
                *newDirection = eRenderDirectionForward;
                *nextFrame = TimeValue(firstFrame + frameStep);
            }
            break;
        case ePlaybackModeOnce:
        default:
            if (direction == eRenderDirectionForward) {
                return false;
            } else {
                *nextFrame = TimeValue(lastFrame - frameStep);
                break;
            }
        }
    } else {
        if (direction == eRenderDirectionForward) {
            *nextFrame = TimeValue(frame + frameStep);
        } else {
            *nextFrame = TimeValue(frame - frameStep);
        }
    }

    return true;
} // OutputSchedulerThreadPrivate::getNextFrameInSequence

void
OutputSchedulerThreadPrivate::getNearestInSequence(RenderDirectionEnum direction,
                                                   TimeValue frame,
                                                   TimeValue firstFrame,
                                                   TimeValue lastFrame,
                                                   TimeValue* nextFrame)
{
    if ( (frame >= firstFrame) && (frame <= lastFrame) ) {
        *nextFrame = frame;
    } else if (frame < firstFrame) {
        if (direction == eRenderDirectionForward) {
            *nextFrame = firstFrame;
        } else {
            *nextFrame = lastFrame;
        }
    } else { // frame > lastFrame
        if (direction == eRenderDirectionForward) {
            *nextFrame = lastFrame;
        } else {
            *nextFrame = firstFrame;
        }
    }
}


void
OutputSchedulerThread::startTasksFromLastStartedFrame()
{
    // Tasks are started on the scheduler thread
    assert(QThread::currentThread() == this);

    TimeValue frame;
    bool canContinue;

    {
        boost::shared_ptr<OutputSchedulerThreadStartArgs> args = _imp->runArgs.lock();

        PlaybackModeEnum pMode = _imp->engine->getPlaybackMode();

        {
            QMutexLocker l(&_imp->lastFrameRequestedMutex);
            frame = _imp->lastFrameRequested;
        }
        if ( (args->firstFrame == args->lastFrame) && (frame == args->firstFrame) ) {
            return;
        }

        RenderDirectionEnum newDirection = args->direction;
        ///If startingTime is already taken into account in the framesToRender, push new frames from the last one in the stack instead
        canContinue = OutputSchedulerThreadPrivate::getNextFrameInSequence(pMode, args->direction, frame,
                                                                           args->firstFrame, args->lastFrame, args->frameStep, &frame, &newDirection);
        if (newDirection != args->direction) {
            args->direction = newDirection;
        }
    }

    if (canContinue) {
        QMutexLocker l(&_imp->renderThreadsMutex);
        startTasks(frame);
    }
} // startTasksFromLastStartedFrame

void
OutputSchedulerThread::startTasks(TimeValue startingFrame)
{
    // Tasks are started on the scheduler thread
    assert(QThread::currentThread() == this);

    boost::shared_ptr<OutputSchedulerThreadStartArgs> args = _imp->runArgs.lock();

    PlaybackModeEnum pMode = _imp->engine->getPlaybackMode();
    if (args->firstFrame == args->lastFrame) {
        RenderThreadTask* task = createRunnable(startingFrame, args->enableRenderStats, args->viewsToRender);
        _imp->startRunnable(task);
        QMutexLocker k(&_imp->lastFrameRequestedMutex);
        _imp->lastFrameRequested = startingFrame;
    } else {

        // For now just run one frame concurrently, it is better to try to render one frame the fastest
        const int nConcurrentFrames = 1;

        TimeValue frame = startingFrame;
        RenderDirectionEnum newDirection = args->direction;

        for (int i = 0; i < nConcurrentFrames; ++i) {

            RenderThreadTask* task = createRunnable(frame, args->enableRenderStats, args->viewsToRender);
            _imp->startRunnable(task);

            {
                QMutexLocker k(&_imp->lastFrameRequestedMutex);
                _imp->lastFrameRequested = frame;
            }

            if ( !OutputSchedulerThreadPrivate::getNextFrameInSequence(pMode, args->direction, frame,
                                                                       args->firstFrame, args->lastFrame, args->frameStep, &frame, &newDirection) ) {
                break;
            }
        }
        if (newDirection != args->direction) {
            args->direction = newDirection;
        }
    }
} // OutputSchedulerThread::startTasks


void
OutputSchedulerThread::notifyThreadAboutToQuit(RenderThreadTask* thread)
{
    QMutexLocker l(&_imp->renderThreadsMutex);
    RenderThreads::iterator found = _imp->getRunnableIterator(thread);

    if ( found != _imp->renderThreads.end() ) {
        found->active = false;
        _imp->renderThreads.erase(found);
        _imp->allRenderThreadsInactiveCond.wakeOne();
    }
}

void
OutputSchedulerThread::startRender()
{
    if ( isFPSRegulationNeeded() ) {
        _imp->timer->playState = ePlayStateRunning;
    }

    // Start measuring
    _imp->renderTimer.reset(new TimeLapse);

    // We will push frame to renders starting at startingFrame.
    // They will be in the range determined by firstFrame-lastFrame
    TimeValue startingFrame;
    TimeValue firstFrame, lastFrame;
    TimeValue frameStep;
    RenderDirectionEnum direction;

    {
        boost::shared_ptr<OutputSchedulerThreadStartArgs> args = _imp->runArgs.lock();

        firstFrame = args->firstFrame;
        lastFrame = args->lastFrame;
        frameStep = args->frameStep;
        startingFrame = timelineGetTime();
        direction = args->direction;
    }
    aboutToStartRender();
    
    // Notify everyone that the render is started
    _imp->engine->s_renderStarted(direction == eRenderDirectionForward);



    ///If the output effect is sequential (only WriteFFMPEG for now)
    EffectInstancePtr effect = _imp->outputEffect.lock();
    WriteNodePtr isWrite = toWriteNode( effect );
    if (isWrite) {
        NodePtr embeddedWriter = isWrite->getEmbeddedWriter();
        if (embeddedWriter) {
            effect = embeddedWriter->getEffectInstance();
        }
    }
    SequentialPreferenceEnum pref = effect->getSequentialPreference();
    if ( (pref == eSequentialPreferenceOnlySequential) || (pref == eSequentialPreferencePreferSequential) ) {
        RenderScale scaleOne(1.);
        if (effect->beginSequenceRender_public(firstFrame,
                                               lastFrame,
                                               frameStep,
                                               false /*interactive*/,
                                               scaleOne,
                                               true /*isSequentialRender*/,
                                               false /*isRenderResponseToUserInteraction*/,
                                               false /*draftMode*/,
                                               ViewIdx(0),
                                               false /*useOpenGL*/,
                                               EffectOpenGLContextDataPtr(),
                                               TreeRenderNodeArgsPtr()) == eStatusFailed) {


            _imp->engine->abortRenderingNoRestart();

            return;
        }
    }

    {
        QMutexLocker k(&_imp->lastFrameRequestedMutex);
        _imp->expectedFrameToRender = startingFrame;
        _imp->schedulerRenderDirection = direction;
    }

    startTasks(startingFrame);


} // OutputSchedulerThread::startRender

void
OutputSchedulerThread::stopRender()
{
    _imp->timer->playState = ePlayStatePause;

    // Remove all current threads so the new render doesn't have many threads concurrently trying to do the same thing at the same time
    _imp->waitForRenderThreadsToQuit();

    ///If the output effect is sequential (only WriteFFMPEG for now)
    EffectInstancePtr effect = _imp->outputEffect.lock();
    WriteNodePtr isWrite = toWriteNode( effect );
    if (isWrite) {
        NodePtr embeddedWriter = isWrite->getEmbeddedWriter();
        if (embeddedWriter) {
            effect = embeddedWriter->getEffectInstance();
        }
    }
    SequentialPreferenceEnum pref = effect->getSequentialPreference();
    if ( (pref == eSequentialPreferenceOnlySequential) || (pref == eSequentialPreferencePreferSequential) ) {
        TimeValue firstFrame, lastFrame, frameStep;

        {
            QMutexLocker k(&_imp->lastRunArgsMutex);
            boost::shared_ptr<OutputSchedulerThreadStartArgs> args = _imp->runArgs.lock();
            firstFrame = args->firstFrame;
            lastFrame = args->lastFrame;
            frameStep = args->frameStep;
        }

        RenderScale scaleOne(1.);
        ignore_result( effect->endSequenceRender_public(firstFrame,
                                                        lastFrame,
                                                        frameStep,
                                                        !appPTR->isBackground(),
                                                        scaleOne, true,
                                                        !appPTR->isBackground(),
                                                        false,
                                                        ViewIdx(0),
                                                        false /*use OpenGL render*/,
                                                        EffectOpenGLContextDataPtr(),
                                                        TreeRenderNodeArgsPtr()) );
    }


    bool wasAborted = isBeingAborted();


    ///Notify everyone that the render is finished
    _imp->engine->s_renderFinished(wasAborted ? 1 : 0);

    onRenderStopped(wasAborted);

    // When playing once disable auto-restart
    if (!wasAborted && _imp->engine->getPlaybackMode() == ePlaybackModeOnce) {
        _imp->engine->setPlaybackAutoRestartEnabled(false);
    }


    {
        QMutexLocker k(&_imp->bufMutex);
        _imp->buf.clear();
    }

    _imp->renderTimer.reset();
} // OutputSchedulerThread::stopRender

GenericSchedulerThread::ThreadStateEnum
OutputSchedulerThread::threadLoopOnce(const ThreadStartArgsPtr &inArgs)
{
    boost::shared_ptr<OutputSchedulerThreadStartArgs> args = boost::dynamic_pointer_cast<OutputSchedulerThreadStartArgs>(inArgs);

    assert(args);
    _imp->runArgs = args;

    ThreadStateEnum state = eThreadStateActive;
    TimeValue expectedTimeToRenderPreviousIteration = TimeValue(INT_MIN);

    // This is the number of time this thread was woken up by a render thread because a frame was available for processing,
    // but this is not the frame this thread expects to render. If it reaches a certain amount, we detected a stall and abort.
    int nbIterationsWithoutProcessing = 0;

    startRender();

    for (;; ) {

        // When set to true, we don't sleep in the bufEmptyCondition but in the startCondition instead, indicating
        //we finished a render
        bool renderFinished = false;

        {
            ///_imp->renderFinished might be set when in FFA scheduling policy
            QMutexLocker l(&_imp->renderFinishedMutex);
            if (_imp->renderFinished) {
                renderFinished = true;
            }
        }
        bool bufferEmpty;
        {
            QMutexLocker l(&_imp->bufMutex);
            bufferEmpty = _imp->buf.empty();
        }

        // This is the frame that the scheduler expects to process now
        TimeValue expectedTimeToRender;

        while (!bufferEmpty) {

            state = resolveState();
            if ( (state == eThreadStateAborted) || (state == eThreadStateStopped) ) {
                // Do not wait in the buf wait condition and go directly into the stopEngine()
                renderFinished = true;
                break;
            }


            {
                QMutexLocker k(&_imp->lastFrameRequestedMutex);
                expectedTimeToRender = _imp->expectedFrameToRender;
            }

#ifdef TRACE_SCHEDULER
            if ( (expectedTimeToRenderPreviousIteration == INT_MIN) || (expectedTimeToRenderPreviousIteration != expectedTimeToRender) ) {
                qDebug() << "Scheduler Thread: waiting for " << expectedTimeToRender << " to be rendered...";
            }
#endif
            if (expectedTimeToRenderPreviousIteration == expectedTimeToRender) {
                ++nbIterationsWithoutProcessing;
                if (nbIterationsWithoutProcessing >= NATRON_SCHEDULER_ABORT_AFTER_X_UNSUCCESSFUL_ITERATIONS) {
#ifdef TRACE_SCHEDULER
                    qDebug() << "Scheduler Thread: Detected stall after " << NATRON_SCHEDULER_ABORT_AFTER_X_UNSUCCESSFUL_ITERATIONS
                             << "unsuccessful iterations";
#endif
                    renderFinished = true;
                    break;
                }
            } else {
                nbIterationsWithoutProcessing = 0;
            }
            boost::shared_ptr<OutputSchedulerThreadExecMTArgs> framesToRender( new OutputSchedulerThreadExecMTArgs() );
            {
                QMutexLocker l(&_imp->bufMutex);
                _imp->getFromBufferAndErase(expectedTimeToRender, framesToRender->frames);
            }

            if ( framesToRender->frames.empty() ) {
                // The expected frame is not yet ready, go to sleep again
                expectedTimeToRenderPreviousIteration = expectedTimeToRender;
                break;
            }

#ifdef TRACE_SCHEDULER
            qDebug() << "Scheduler Thread: received frame to process" << expectedTimeToRender;
#endif

            TimeValue nextFrameToRender(-1.);
            RenderDirectionEnum newDirection = eRenderDirectionForward;

            if (!renderFinished) {
                ///////////
                /////Refresh frame range if needed (for viewers)


                TimeValue firstFrame, lastFrame;
                getFrameRangeToRender(firstFrame, lastFrame);


                RenderDirectionEnum timelineDirection;
                TimeValue frameStep;

                // Refresh the firstframe/lastFrame as they might have changed on the timeline
                args->firstFrame = firstFrame;
                args->lastFrame = lastFrame;


                timelineDirection = _imp->schedulerRenderDirection;
                frameStep = args->frameStep;


                ///////////
                ///Determine if we finished rendering or if we should just increment/decrement the timeline
                ///or just loop/bounce
                PlaybackModeEnum pMode = _imp->engine->getPlaybackMode();
                if ( (firstFrame == lastFrame) && (pMode == ePlaybackModeOnce) ) {
                    renderFinished = true;
                    newDirection = eRenderDirectionForward;
                } else {
                    renderFinished = !OutputSchedulerThreadPrivate::getNextFrameInSequence(pMode, timelineDirection,
                                                                                           expectedTimeToRender, firstFrame,
                                                                                           lastFrame, frameStep, &nextFrameToRender, &newDirection);
                }

                if (newDirection != timelineDirection) {
                    _imp->schedulerRenderDirection = newDirection;
                }

                if (!renderFinished) {
                    {
                        QMutexLocker k(&_imp->lastFrameRequestedMutex);
                        _imp->expectedFrameToRender = nextFrameToRender;
                    }


                    startTasksFromLastStartedFrame();

                }
            } // if (!renderFinished) {

            if (_imp->timer->playState == ePlayStateRunning) {
                _imp->timer->waitUntilNextFrameIsDue(); // timer synchronizing with the requested fps
            }


            state = resolveState();
            if ( (state == eThreadStateAborted) || (state == eThreadStateStopped) ) {
                // Do not wait in the buf wait condition and go directly into the stopEngine()
                renderFinished = true;
                break;
            }

            if (_imp->mode == eProcessFrameBySchedulerThread) {
                processFrame(framesToRender->frames);
            } else {
                requestExecutionOnMainThread(framesToRender);
            }

            expectedTimeToRenderPreviousIteration = expectedTimeToRender;

#ifdef TRACE_SCHEDULER
            QString pushDirectionStr = newDirection == eRenderDirectionForward ? QLatin1String("Forward") : QLatin1String("Backward");
            qDebug() << "Scheduler Thread: Frame " << expectedTimeToRender << " processed, setting expectedTimeToRender to " << nextFrameToRender
                     << ", new process direction is " << pushDirectionStr;
#endif
            if (!renderFinished) {
                ///Timeline might have changed if another thread moved the playhead
                TimeValue timelineCurrentTime = timelineGetTime();
                if (timelineCurrentTime != expectedTimeToRender) {
                    timelineGoTo(timelineCurrentTime);
                } else {
                    timelineGoTo(nextFrameToRender);
                }
            }

            ////////////
            /////At this point the frame has been processed by the output device

            assert( !framesToRender->frames.empty() );
            {
                const BufferedFrame& frame = framesToRender->frames.front();
                std::vector<ViewIdx> views(1);
                views[0] = frame.view;
                notifyFrameRendered(expectedTimeToRender, frame.view, views, frame.stats, eSchedulingPolicyOrdered);
            }

            ///////////
            /// End of the loop, refresh bufferEmpty
            {
                QMutexLocker l(&_imp->bufMutex);
                bufferEmpty = _imp->buf.empty();
            }
        } // while(!bufferEmpty)

        if (state == eThreadStateActive) {
            state = resolveState();
        }

        if ( (state == eThreadStateAborted) || (state == eThreadStateStopped) ) {
            renderFinished = true;
        }

        if (!renderFinished) {
            assert(state == eThreadStateActive);
            QMutexLocker bufLocker (&_imp->bufMutex);
            // Wait here for more frames to be rendered, we will be woken up once appendToBuffer is called
            _imp->bufEmptyCondition.wait(&_imp->bufMutex);
        } else {
            if ( !_imp->engine->isPlaybackAutoRestartEnabled() ) {
                //Move the timeline to the last rendered frame to keep it in sync with what is displayed
                timelineGoTo( getLastRenderedTime() );
            }
            break;
        }
    } // for (;;)

    stopRender();

    return state;
} // OutputSchedulerThread::threadLoopOnce

void
OutputSchedulerThread::onAbortRequested(bool /*keepOldestRender*/)
{
    {
        QMutexLocker l(&_imp->renderThreadsMutex);
        for (RenderThreads::iterator it = _imp->renderThreads.begin(); it != _imp->renderThreads.end(); ++it) {
            AbortableThread* isAbortableThread = dynamic_cast<AbortableThread*>(it->thread);
            if (isAbortableThread) {
                TreeRenderPtr render =isAbortableThread->getCurrentRender();
                if (render) {
                    render->setAborted();
                }
            }
        }
    }

    // If the scheduler is asleep waiting for the buffer to be filling up, we post a fake request
    // that will not be processed anyway because the first thing it does is checking for abort
    {
        QMutexLocker l2(&_imp->bufMutex);
        _imp->bufEmptyCondition.wakeOne();
    }
}

void
OutputSchedulerThread::executeOnMainThread(const ExecOnMTArgsPtr& inArgs)
{
    OutputSchedulerThreadExecMTArgs* args = dynamic_cast<OutputSchedulerThreadExecMTArgs*>( inArgs.get() );

    assert(args);
    processFrame(args->frames);
}

void
OutputSchedulerThread::notifyFrameRendered(int frame,
                                           ViewIdx viewIndex,
                                           const std::vector<ViewIdx>& viewsToRender,
                                           const RenderStatsPtr& stats,
                                           SchedulingPolicyEnum policy)
{
    assert(viewsToRender.size() > 0);

    bool isLastView = viewIndex == viewsToRender[viewsToRender.size() - 1] || viewIndex == -1;

    // Report render stats if desired
    OutputEffectInstancePtr effect = _imp->outputEffect.lock();
    if (stats) {
        double timeSpentForFrame;
        std::map<NodePtr, NodeRenderStats > statResults = stats->getStats(&timeSpentForFrame);
        if ( !statResults.empty() ) {
            effect->reportStats(frame, viewIndex, timeSpentForFrame, statResults);
        }
    }


    bool isBackground = appPTR->isBackground();
    boost::shared_ptr<OutputSchedulerThreadStartArgs> runArgs = _imp->runArgs.lock();
    assert(runArgs);

    // If FFA all parallel renders call render on the Writer in their own thread,
    // otherwise the OutputSchedulerThread thread calls the render of the Writer.
    U64 nbTotalFrames;
    U64 nbFramesRendered;
    //bool renderingIsFinished = false;
    if (policy == eSchedulingPolicyFFA) {
        {
            QMutexLocker l(&_imp->renderFinishedMutex);
            if (isLastView) {
                ++_imp->nFramesRendered;
            }
            nbTotalFrames = std::ceil( (double)(runArgs->lastFrame - runArgs->firstFrame + 1) / runArgs->frameStep );
            nbFramesRendered = _imp->nFramesRendered;


            if (_imp->nFramesRendered == nbTotalFrames) {
                _imp->renderFinished = true;
                l.unlock();

                // Notify the scheduler rendering is finished by append a fake frame to the buffer
                {
                    QMutexLocker bufLocker (&_imp->bufMutex);
                    _imp->appendBufferedFrame( TimeValue(0), viewIndex, RenderStatsPtr(), BufferableObjectPtr() );
                    _imp->bufEmptyCondition.wakeOne();
                }
            }
        }
    } else {
        nbTotalFrames = std::floor( (double)(runArgs->lastFrame - runArgs->firstFrame + 1) / runArgs->frameStep );
        if (runArgs->direction == eRenderDirectionForward) {
            nbFramesRendered = (frame - runArgs->firstFrame) / runArgs->frameStep;
        } else {
            nbFramesRendered = (runArgs->lastFrame - frame) / runArgs->frameStep;
        }
    } // if (policy == eSchedulingPolicyFFA) {

    double percentage = 0.;
    assert(nbTotalFrames > 0);
    if (nbTotalFrames != 0) {
        QMutexLocker k(&_imp->renderFinishedMutex);
        percentage = (double)_imp->nFramesRendered / nbTotalFrames;
    }
    assert(_imp->renderTimer);
    double timeSpentSinceStartSec = _imp->renderTimer->getTimeSinceCreation();
    double estimatedFps = (double)nbFramesRendered / timeSpentSinceStartSec;
    double timeRemaining = timeSpentSinceStartSec * (1. - percentage);

    // If running in background, notify to the pipe that we rendered a frame
    if (isBackground) {
        QString longMessage;
        QTextStream ts(&longMessage);
        QString frameStr = QString::number(frame);
        QString fpsStr = QString::number(estimatedFps, 'f', 1);
        QString percentageStr = QString::number(percentage * 100, 'f', 1);
        QString timeRemainingStr = Timer::printAsTime(timeRemaining, true);

        ts << effect->getScriptName_mt_safe().c_str() << tr(" ==> Frame: ");
        ts << frameStr << tr(", Progress: ") << percentageStr << "%, " << fpsStr << tr(" Fps, Time Remaining: ") << timeRemainingStr;

        QString shortMessage = QString::fromUtf8(kFrameRenderedStringShort) + frameStr + QString::fromUtf8(kProgressChangedStringShort) + QString::number(percentage);
        {
            QMutexLocker l(&_imp->bufferedOutputMutex);
            std::string toPrint = longMessage.toStdString();
            if ( (_imp->lastBufferedOutputSize != 0) && ( _imp->lastBufferedOutputSize > longMessage.size() ) ) {
                int nSpacesToAppend = _imp->lastBufferedOutputSize - longMessage.size();
                toPrint.append(nSpacesToAppend, ' ');
            }
            //std::cout << '\r';
            std::cout << toPrint;
            std::cout << std::endl;

            /*std::cout.flush();
               if (renderingIsFinished) {
                std::cout << std::endl;
               }*/
            _imp->lastBufferedOutputSize = longMessage.size();
        }

        appPTR->writeToOutputPipe(longMessage, shortMessage, false);
    }

    // Notify we rendered a frame
    if (isLastView) {
        _imp->engine->s_frameRendered(frame, percentage);
    }

    // Call Python after frame ranedered callback
    if ( isLastView && effect->isWriter() ) {
        std::string cb = effect->getNode()->getAfterFrameRenderCallback();
        if ( !cb.empty() ) {
            std::vector<std::string> args;
            std::string error;
            try {
                NATRON_PYTHON_NAMESPACE::getFunctionArguments(cb, &error, &args);
            } catch (const std::exception& e) {
                effect->getApp()->appendToScriptEditor( std::string("Failed to run onFrameRendered callback: ")
                                                        + e.what() );

                return;
            }

            if ( !error.empty() ) {
                effect->getApp()->appendToScriptEditor("Failed to run after frame render callback: " + error);

                return;
            }

            std::string signatureError;
            signatureError.append("The after frame render callback supports the following signature(s):\n");
            signatureError.append("- callback(frame, thisNode, app)");
            if (args.size() != 3) {
                effect->getApp()->appendToScriptEditor("Failed to run after frame render callback: " + signatureError);

                return;
            }

            if ( (args[0] != "frame") || (args[1] != "thisNode") || (args[2] != "app") ) {
                effect->getApp()->appendToScriptEditor("Failed to run after frame render callback: " + signatureError);

                return;
            }

            std::stringstream ss;
            std::string appStr = effect->getApp()->getAppIDString();
            std::string outputNodeName = appStr + "." + effect->getNode()->getFullyQualifiedName();
            ss << cb << "(" << frame << ", ";
            ss << outputNodeName << ", " << appStr << ")";
            std::string script = ss.str();
            try {
                runCallbackWithVariables( QString::fromUtf8( script.c_str() ) );
            } catch (const std::exception& e) {
                notifyRenderFailure( e.what() );

                return;
            }
        }
    }
} // OutputSchedulerThread::notifyFrameRendered

void
OutputSchedulerThread::appendToBuffer_internal(TimeValue time,
                                               ViewIdx view,
                                               const RenderStatsPtr& stats,
                                               const BufferableObjectPtr& frame,
                                               bool wakeThread)
{
    if ( QThread::currentThread() == qApp->thread() ) {
        ///Single-threaded , call directly the function
        if (frame) {
            BufferedFrame b;
            b.time = time;
            b.view = view;
            b.frame = frame;
            BufferedFrames frames;
            frames.push_back(b);
            processFrame(frames);
        }
    } else {
        ///Called by the scheduler thread when an image is rendered

        QMutexLocker l(&_imp->bufMutex);
        _imp->appendBufferedFrame(time, view, stats, frame);
        if (wakeThread) {
            ///Wake up the scheduler thread that an image is available if it is asleep so it can process it.
            _imp->bufEmptyCondition.wakeOne();
        }
    }
}

void
OutputSchedulerThread::appendToBuffer(TimeValue time,
                                      ViewIdx view,
                                      const RenderStatsPtr& stats,
                                      const BufferableObjectPtr& image)
{
    appendToBuffer_internal(time, view, stats, image, true);
}

void
OutputSchedulerThread::appendToBuffer(TimeValue time,
                                      ViewIdx view,
                                      const RenderStatsPtr& stats,
                                      const BufferableObjectList& frames)
{
    if ( frames.empty() ) {
        return;
    }
    BufferableObjectList::const_iterator next = frames.begin();
    if ( next != frames.end() ) {
        ++next;
    }
    for (BufferableObjectList::const_iterator it = frames.begin(); it != frames.end(); ++it) {
        if ( next != frames.end() ) {
            appendToBuffer_internal(time, view, stats, *it, false);
            ++next;
        } else {
            appendToBuffer_internal(time, view, stats, *it, true);
        }
    }
}

void
OutputSchedulerThread::setDesiredFPS(double d)
{
    _imp->timer->setDesiredFrameRate(d);
}

double
OutputSchedulerThread::getDesiredFPS() const
{
    return _imp->timer->getDesiredFrameRate();
}

void
OutputSchedulerThread::getLastRunArgs(RenderDirectionEnum* direction,
                                      std::vector<ViewIdx>* viewsToRender) const
{
    QMutexLocker k(&_imp->lastRunArgsMutex);

    *direction = _imp->lastPlaybackRenderDirection;
    *viewsToRender = _imp->lastPlaybackViewsToRender;
}

void
OutputSchedulerThread::renderFrameRange(bool isBlocking,
                                        bool enableRenderStats,
                                        TimeValue firstFrame,
                                        TimeValue lastFrame,
                                        TimeValue frameStep,
                                        const std::vector<ViewIdx>& viewsToRender,
                                        RenderDirectionEnum direction)
{
    {
        QMutexLocker k(&_imp->lastRunArgsMutex);
        _imp->lastPlaybackRenderDirection = direction;
        _imp->lastPlaybackViewsToRender = viewsToRender;
    }
    if (direction == eRenderDirectionForward) {
        timelineGoTo(firstFrame);
    } else {
        timelineGoTo(lastFrame);
    }

    boost::shared_ptr<OutputSchedulerThreadStartArgs> args( new OutputSchedulerThreadStartArgs(isBlocking, enableRenderStats, firstFrame, lastFrame, frameStep, viewsToRender, direction) );

    {
        QMutexLocker k(&_imp->renderFinishedMutex);
        _imp->nFramesRendered = 0;
        _imp->renderFinished = false;
    }

    startTask(args);
}

void
OutputSchedulerThread::renderFromCurrentFrame(bool enableRenderStats,
                                              const std::vector<ViewIdx>& viewsToRender,
                                              RenderDirectionEnum timelineDirection)
{
    {
        QMutexLocker k(&_imp->lastRunArgsMutex);
        _imp->lastPlaybackRenderDirection = timelineDirection;
        _imp->lastPlaybackViewsToRender = viewsToRender;
    }
    TimeValue firstFrame, lastFrame;
    getFrameRangeToRender(firstFrame, lastFrame);

    ///Make sure current frame is in the frame range
    TimeValue currentTime = timelineGetTime();

    OutputSchedulerThreadPrivate::getNearestInSequence(timelineDirection, currentTime, firstFrame, lastFrame, &currentTime);

    boost::shared_ptr<OutputSchedulerThreadStartArgs> args( new OutputSchedulerThreadStartArgs(false, enableRenderStats, firstFrame, lastFrame, TimeValue(1), viewsToRender, timelineDirection) );
    startTask(args);
}

void
OutputSchedulerThread::notifyRenderFailure(const std::string& errorMessage)
{
    ///Abort all ongoing rendering
    boost::shared_ptr<OutputSchedulerThreadStartArgs> args = _imp->runArgs.lock();

    assert(args);


    ///Handle failure: for viewers we make it black and don't display the error message which is irrelevant
    handleRenderFailure(errorMessage);

    _imp->engine->abortRenderingNoRestart();

    if (args->isBlocking) {
        waitForAbortToComplete_enforce_blocking();
    }
}

boost::shared_ptr<OutputSchedulerThreadStartArgs>
OutputSchedulerThread::getCurrentRunArgs() const
{
    return _imp->runArgs.lock();
}

int
OutputSchedulerThread::getNRenderThreads() const
{
    QMutexLocker l(&_imp->renderThreadsMutex);

    return (int)_imp->renderThreads.size();
}

int
OutputSchedulerThread::getNActiveRenderThreads() const
{
    QMutexLocker l(&_imp->renderThreadsMutex);

    return _imp->getNActiveRenderThreads();
}



RenderEngine*
OutputSchedulerThread::getEngine() const
{
    return _imp->engine;
}

void
OutputSchedulerThread::runCallbackWithVariables(const QString& callback)
{
    if ( !callback.isEmpty() ) {
        OutputEffectInstancePtr effect = _imp->outputEffect.lock();
        QString script = callback;
        std::string appID = effect->getApp()->getAppIDString();
        std::string nodeName = effect->getNode()->getFullyQualifiedName();
        std::string nodeFullName = appID + "." + nodeName;
        script.append( QString::fromUtf8( nodeFullName.c_str() ) );
        script.append( QLatin1Char(',') );
        script.append( QString::fromUtf8( appID.c_str() ) );
        script.append( QString::fromUtf8(")\n") );

        std::string err, output;
        if ( !NATRON_PYTHON_NAMESPACE::interpretPythonScript(callback.toStdString(), &err, &output) ) {
            effect->getApp()->appendToScriptEditor("Failed to run callback: " + err);
            throw std::runtime_error(err);
        } else if ( !output.empty() ) {
            effect->getApp()->appendToScriptEditor(output);
        }
    }
}

////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////
//////////////////////// RenderThreadTask ////////////

struct RenderThreadTaskPrivate
{
    OutputSchedulerThread* scheduler;
    boost::weak_ptr<OutputEffectInstance> output;


    TimeValue time;
    bool useRenderStats;
    std::vector<ViewIdx> viewsToRender;


    RenderThreadTaskPrivate(const OutputEffectInstancePtr& output,
                            OutputSchedulerThread* scheduler,
                            const TimeValue time,
                            const bool useRenderStats,
                            const std::vector<ViewIdx>& viewsToRender
                            )
        : scheduler(scheduler)
        , output(output)
        , time(time)
        , useRenderStats(useRenderStats)
        , viewsToRender(viewsToRender)
    {
    }
};



RenderThreadTask::RenderThreadTask(const OutputEffectInstancePtr& output,
                                   OutputSchedulerThread* scheduler,
                                   const TimeValue time,
                                   const bool useRenderStats,
                                   const std::vector<ViewIdx>& viewsToRender)
    : QRunnable()
    , _imp( new RenderThreadTaskPrivate(output, scheduler, time, useRenderStats, viewsToRender) )
{
}

RenderThreadTask::~RenderThreadTask()
{
}

void
RenderThreadTask::run()
{
    renderFrame(_imp->time, _imp->viewsToRender, _imp->useRenderStats);
    _imp->scheduler->notifyThreadAboutToQuit(this);
}


////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////
//////////////////////// DefaultScheduler ////////////


DefaultScheduler::DefaultScheduler(RenderEngine* engine,
                                   const OutputEffectInstancePtr& effect)
    : OutputSchedulerThread(engine, effect, eProcessFrameBySchedulerThread)
    , _effect(effect)
    , _currentTimeMutex()
    , _currentTime(0)
{
    engine->setPlaybackMode(ePlaybackModeOnce);
}

DefaultScheduler::~DefaultScheduler()
{
}

class DefaultRenderFrameRunnable
    : public RenderThreadTask
{
public:



    DefaultRenderFrameRunnable(const OutputEffectInstancePtr& writer,
                               OutputSchedulerThread* scheduler,
                               const TimeValue time,
                               const bool useRenderStats,
                               const std::vector<ViewIdx>& viewsToRender)
        : RenderThreadTask(writer, scheduler, time, useRenderStats, viewsToRender)
    {
    }



    virtual ~DefaultRenderFrameRunnable()
    {
    }

private:

    void runBeforeFrameRenderCallback(TimeValue frame, const NodePtr& outputNode)
    {
        std::string cb = outputNode->getBeforeFrameRenderCallback();
        if ( cb.empty() ) {
            return;
        }
        std::vector<std::string> args;
        std::string error;
        try {
            NATRON_PYTHON_NAMESPACE::getFunctionArguments(cb, &error, &args);
        } catch (const std::exception& e) {
            outputNode->getApp()->appendToScriptEditor( std::string("Failed to run beforeFrameRendered callback: ")
                                                   + e.what() );

            return;
        }

        if ( !error.empty() ) {
            outputNode->getApp()->appendToScriptEditor("Failed to run before frame render callback: " + error);

            return;
        }

        std::string signatureError;
        signatureError.append("The before frame render callback supports the following signature(s):\n");
        signatureError.append("- callback(frame, thisNode, app)");
        if (args.size() != 3) {
            outputNode->getApp()->appendToScriptEditor("Failed to run before frame render callback: " + signatureError);

            return;
        }

        if ( (args[0] != "frame") || (args[1] != "thisNode") || (args[2] != "app") ) {
            outputNode->getApp()->appendToScriptEditor("Failed to run before frame render callback: " + signatureError);

            return;
        }

        std::stringstream ss;
        std::string appStr = outputNode->getApp()->getAppIDString();
        std::string outputNodeName = appStr + "." + outputNode->getFullyQualifiedName();
        ss << cb << "(" << frame << ", " << outputNodeName << ", " << appStr << ")";
        std::string script = ss.str();
        try {
            _imp->scheduler->runCallbackWithVariables( QString::fromUtf8( script.c_str() ) );
        } catch (const std::exception &e) {
            _imp->scheduler->notifyRenderFailure( e.what() );

            return;
        }


    }


    virtual void renderFrame(TimeValue time,
                             const std::vector<ViewIdx>& viewsToRender,
                             bool enableRenderStats)
    {
        OutputEffectInstancePtr output = _imp->output.lock();

        if (!output) {
            _imp->scheduler->notifyRenderFailure("");

            return;
        }

        // Even if enableRenderStats is false, we at least profile the time spent rendering the frame when rendering with a Write node.
        // Though we don't enable render stats for sequential renders (e.g: WriteFFMPEG) since this is 1 file.
        RenderStatsPtr stats( new RenderStats(enableRenderStats) );

        NodePtr outputNode = output->getNode();

        // Notify we start rendering a frame to Python
        runBeforeFrameRenderCallback(time, outputNode);

        try {
            // Writers always render at scale 1 (for now)
            int mipMapLevel = 0;
            RenderScale scale(1.);

            RectD rod;

            EffectInstancePtr activeInputToRender = output;

            // If the output is a Write node, actually write is the internal write node encoder
            WriteNodePtr isWrite = toWriteNode(output);
            if (isWrite) {
                NodePtr embeddedWriter = isWrite->getEmbeddedWriter();
                if (embeddedWriter) {
                    activeInputToRender = embeddedWriter->getEffectInstance();
                }
            }
            assert(activeInputToRender);

            NodePtr activeInputNode = activeInputToRender->getNode();

            for (std::size_t view = 0; view < viewsToRender.size(); ++view) {


                // Get layers to render
                std::list<ImageComponents> components;

                //Use needed components to figure out what we need to render
                ComponentsNeededMap neededComps;
                bool processAll;
                SequenceTime ptTime;
                int ptView;
                std::bitset<4> processChannels;
                NodePtr ptInput;
                activeInputToRender->getComponentsNeededAndProduced_public(true, true, time, viewsToRender[view], &neededComps, &processAll, &ptTime, &ptView, &processChannels, &ptInput);
                components.clear();

                ComponentsNeededMap::iterator foundOutput = neededComps.find(-1);
                if ( foundOutput != neededComps.end() ) {
                    for (std::size_t j = 0; j < foundOutput->second.size(); ++j) {
                        components.push_back(foundOutput->second[j]);
                    }
                }

                std::map<ImageComponents, ImagePtr> planes;
                RenderRoIRetCode retCode = activeInputNode->renderFrame(time, viewsToRender[view], mipMapLevel, true /*isPlayback*/, 0, components, &planes);

                if (retCode != eRenderRoIRetCodeOk) {
                    if (retCode == eRenderRoIRetCodeAborted) {
                        _imp->scheduler->notifyRenderFailure("Render aborted");
                    } else {
                        _imp->scheduler->notifyRenderFailure("Error caught while rendering");
                    }

                    return;
                }

                // If we need sequential rendering, pass the image to the output scheduler that will ensure the sequential ordering
                _imp->scheduler->notifyFrameRendered(time, viewsToRender[view], viewsToRender, stats, eSchedulingPolicyFFA);
            }
        } catch (const std::exception& e) {
            _imp->scheduler->notifyRenderFailure( std::string("Error while rendering: ") + e.what() );
        }
    } // renderFrame
};


RenderThreadTask*
DefaultScheduler::createRunnable(TimeValue frame,
                                 bool useRenderStarts,
                                 const std::vector<ViewIdx>& viewsToRender)
{
    return new DefaultRenderFrameRunnable(_effect.lock(), this, frame, useRenderStarts, viewsToRender);
}



/**
 * @brief Called whenever there are images available to process in the buffer.
 * Once processed, the frame will be removed from the buffer.
 *
 * According to the ProcessFrameModeEnum given to the scheduler this function will be called either by the scheduler thread (this)
 * or by the application's main-thread (typically to do OpenGL rendering).
 **/
void
DefaultScheduler::processFrame(const BufferedFrames& /*frames*/)
{
    // We don't have anymore writer that need to process things in order. WriteFFMPEG is doing it for us
} // DefaultScheduler::processFrame


void
DefaultScheduler::timelineGoTo(TimeValue time)
{
    QMutexLocker k(&_currentTimeMutex);

    _currentTime =  time;
}

TimeValue
DefaultScheduler::timelineGetTime() const
{
    QMutexLocker k(&_currentTimeMutex);

    return _currentTime;
}

void
DefaultScheduler::getFrameRangeToRender(TimeValue& first,
                                        TimeValue& last) const
{
    boost::shared_ptr<OutputSchedulerThreadStartArgs> args = getCurrentRunArgs();

    first = args->firstFrame;
    last = args->lastFrame;
}

void
DefaultScheduler::handleRenderFailure(const std::string& errorMessage)
{
    if ( appPTR->isBackground() ) {
        std::cerr << errorMessage << std::endl;
    }
}

SchedulingPolicyEnum
DefaultScheduler::getSchedulingPolicy() const
{
    return eSchedulingPolicyFFA;
}

void
DefaultScheduler::aboutToStartRender()
{
    boost::shared_ptr<OutputSchedulerThreadStartArgs> args = getCurrentRunArgs();
    OutputEffectInstancePtr effect = _effect.lock();

    {
        QMutexLocker k(&_currentTimeMutex);
        if (args->direction == eRenderDirectionForward) {
            _currentTime  = args->firstFrame;
        } else {
            _currentTime  = args->lastFrame;
        }
    }
    bool isBackGround = appPTR->isBackground();

    if (!isBackGround) {
        effect->setKnobsFrozen(true);
    } else {
        QString longText = QString::fromUtf8( effect->getScriptName_mt_safe().c_str() ) + tr(" ==> Rendering started");
        appPTR->writeToOutputPipe(longText, QString::fromUtf8(kRenderingStartedShort), true);
    }

    // Activate the internal writer node for a write node
    WriteNodePtr isWrite = toWriteNode( effect );
    if (isWrite) {
        isWrite->onSequenceRenderStarted();
    }

    std::string cb = effect->getNode()->getBeforeRenderCallback();
    if ( !cb.empty() ) {
        std::vector<std::string> args;
        std::string error;
        try {
            NATRON_PYTHON_NAMESPACE::getFunctionArguments(cb, &error, &args);
        } catch (const std::exception& e) {
            effect->getApp()->appendToScriptEditor( std::string("Failed to run beforeRender callback: ")
                                                    + e.what() );

            return;
        }

        if ( !error.empty() ) {
            effect->getApp()->appendToScriptEditor("Failed to run beforeRender callback: " + error);

            return;
        }

        std::string signatureError;
        signatureError.append("The beforeRender callback supports the following signature(s):\n");
        signatureError.append("- callback(thisNode, app)");
        if (args.size() != 2) {
            effect->getApp()->appendToScriptEditor("Failed to run beforeRender callback: " + signatureError);

            return;
        }

        if ( (args[0] != "thisNode") || (args[1] != "app") ) {
            effect->getApp()->appendToScriptEditor("Failed to run beforeRender callback: " + signatureError);

            return;
        }


        std::stringstream ss;
        std::string appStr = effect->getApp()->getAppIDString();
        std::string outputNodeName = appStr + "." + effect->getNode()->getFullyQualifiedName();
        ss << cb << "(" << outputNodeName << ", " << appStr << ")";
        std::string script = ss.str();
        try {
            runCallbackWithVariables( QString::fromUtf8( script.c_str() ) );
        } catch (const std::exception &e) {
            notifyRenderFailure( e.what() );
        }
    }
} // DefaultScheduler::aboutToStartRender

void
DefaultScheduler::onRenderStopped(bool aborted)
{
    OutputEffectInstancePtr effect = _effect.lock();
    bool isBackGround = appPTR->isBackground();

    if (!isBackGround) {
        effect->setKnobsFrozen(false);
    }

    {
        QString longText = QString::fromUtf8( effect->getScriptName_mt_safe().c_str() ) + tr(" ==> Rendering finished");
        appPTR->writeToOutputPipe(longText, QString::fromUtf8(kRenderingFinishedStringShort), true);
    }

    effect->notifyRenderFinished();

    std::string cb = effect->getNode()->getAfterRenderCallback();
    if ( !cb.empty() ) {
        std::vector<std::string> args;
        std::string error;
        try {
            NATRON_PYTHON_NAMESPACE::getFunctionArguments(cb, &error, &args);
        } catch (const std::exception& e) {
            effect->getApp()->appendToScriptEditor( std::string("Failed to run afterRender callback: ")
                                                    + e.what() );

            return;
        }

        if ( !error.empty() ) {
            effect->getApp()->appendToScriptEditor("Failed to run afterRender callback: " + error);

            return;
        }

        std::string signatureError;
        signatureError.append("The after render callback supports the following signature(s):\n");
        signatureError.append("- callback(aborted, thisNode, app)");
        if (args.size() != 3) {
            effect->getApp()->appendToScriptEditor("Failed to run afterRender callback: " + signatureError);

            return;
        }

        if ( (args[0] != "aborted") || (args[1] != "thisNode") || (args[2] != "app") ) {
            effect->getApp()->appendToScriptEditor("Failed to run afterRender callback: " + signatureError);

            return;
        }


        std::stringstream ss;
        std::string appStr = effect->getApp()->getAppIDString();
        std::string outputNodeName = appStr + "." + effect->getNode()->getFullyQualifiedName();
        ss << cb << "(";
        if (aborted) {
            ss << "True, ";
        } else {
            ss << "False, ";
        }
        ss << outputNodeName << ", " << appStr << ")";
        std::string script = ss.str();
        try {
            runCallbackWithVariables( QString::fromUtf8( script.c_str() ) );
        } catch (...) {
            //Ignore expcetions in callback since the render is finished anyway
        }
    }
} // DefaultScheduler::onRenderStopped

////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////
//////////////////////// ViewerDisplayScheduler ////////////


ViewerDisplayScheduler::ViewerDisplayScheduler(RenderEngine* engine,
                                               const boost::shared_ptr<ViewerInstance>& viewer)
    : OutputSchedulerThread(engine, viewer, eProcessFrameByMainThread) //< OpenGL rendering is done on the main-thread
    , _viewer(viewer)
{
}

ViewerDisplayScheduler::~ViewerDisplayScheduler()
{
}

/**
 * @brief Called whenever there are images available to process in the buffer.
 * Once processed, the frame will be removed from the buffer.
 *
 * According to the ProcessFrameModeEnum given to the scheduler this function will be called either by the scheduler thread (this)
 * or by the application's main-thread (typically to do OpenGL rendering).
 **/
void
ViewerDisplayScheduler::processFrame(const BufferedFrames& frames)
{
    boost::shared_ptr<ViewerInstance> viewer = _viewer.lock();

    if ( !frames.empty() ) {
        viewer->aboutToUpdateTextures();
    }
    if ( !frames.empty() ) {
        for (BufferedFrames::const_iterator it = frames.begin(); it != frames.end(); ++it) {
            boost::shared_ptr<UpdateViewerParams> params = boost::dynamic_pointer_cast<UpdateViewerParams>(it->frame);
            assert(params);
            viewer->updateViewer(params);
        }
        viewer->redrawViewerNow();
    } else {
        viewer->redrawViewer();
    }
}


void
ViewerDisplayScheduler::timelineGoTo(TimeValue time)
{
    ViewerInstancePtr viewer = _viewer.lock();

    viewer->getTimeline()->seekFrame(time, true, viewer, eTimelineChangeReasonPlaybackSeek);
}

TimeValue
ViewerDisplayScheduler::timelineGetTime() const
{
    return TimeValue(_viewer.lock()->getTimeline()->currentFrame());
}

void
ViewerDisplayScheduler::getFrameRangeToRender(TimeValue &first,
                                              TimeValue &last) const
{
    boost::shared_ptr<ViewerInstance> viewer = _viewer.lock();
    ViewerInstancePtr leadViewer = viewer->getApp()->getLastViewerUsingTimeline();
    ViewerInstancePtr v = leadViewer ? leadViewer : viewer;

    assert(v);
    int left, right;
    v->getTimelineBounds(&left, &right);
    first = TimeValue(left);
    last = TimeValue(right);
}

class ViewerRenderFrameRunnable
    : public RenderThreadTask
{
    boost::weak_ptr<ViewerInstance> _viewer;

public:

    ViewerRenderFrameRunnable(const boost::shared_ptr<ViewerInstance>& viewer,
                              OutputSchedulerThread* scheduler,
                              const TimeValue frame,
                              const bool useRenderStarts,
                              const std::vector<ViewIdx>& viewsToRender)
        : RenderThreadTask(viewer, scheduler, frame, useRenderStarts, viewsToRender)
        , _viewer(viewer)
    {
    }



    virtual ~ViewerRenderFrameRunnable()
    {
    }

private:

    virtual void renderFrame(TimeValue time,
                             const std::vector<ViewIdx>& viewsToRender,
                             bool enableRenderStats)
    {
        RenderStatsPtr stats;

        if (enableRenderStats) {
            stats.reset( new RenderStats(enableRenderStats) );
        }
        // The viewer always uses the scheduler thread to regulate the output rate, @see ViewerInstance::renderViewer_internal
        // it calls appendToBuffer by itself
        ViewerInstance::ViewerRenderRetCode stat = ViewerInstance::eViewerRenderRetCodeRedraw;

        //Viewer can only render 1 view for now
        assert(viewsToRender.size() == 1);
        ViewIdx view = viewsToRender.front();
        boost::shared_ptr<ViewerInstance> viewer = _viewer.lock();
        boost::shared_ptr<ViewerArgs> args[2];
        ViewerInstance::ViewerRenderRetCode status[2] = {
            ViewerInstance::eViewerRenderRetCodeFail, ViewerInstance::eViewerRenderRetCodeFail
        };
        bool clearTexture[2] = { false, false };
        BufferableObjectList toAppend;

        for (int i = 0; i < 2; ++i) {
            args[i].reset(new ViewerArgs);
            status[i] = viewer->getRenderViewerArgsAndCheckCache_public(time, true /*isSequential*/, view, i /*inputIndex (A or B)*/, true /*canAbort*/, RotoStrokeItemPtr() /*activeStroke*/, stats, args[i].get());
            clearTexture[i] = status[i] == ViewerInstance::eViewerRenderRetCodeFail || status[i] == ViewerInstance::eViewerRenderRetCodeBlack;
            if (status[i] == ViewerInstance::eViewerRenderRetCodeFail) {
                //Just clear the viewer, nothing to do
                args[i]->params.reset();
                args[i].reset();
            } else if (status[i] == ViewerInstance::eViewerRenderRetCodeBlack) {
                if (args[i]->params) {
                    args[i]->params->tiles.clear();
                    toAppend.push_back(args[i]->params);
                    args[i]->params.reset();
                }
                args[i].reset();
            }
        }

        if ( (status[0] == ViewerInstance::eViewerRenderRetCodeFail) && (status[1] == ViewerInstance::eViewerRenderRetCodeFail) ) {
            viewer->disconnectViewer();
            return;
        }

        if (clearTexture[0]) {
            viewer->disconnectTexture(0, status[0] == ViewerInstance::eViewerRenderRetCodeFail);
        }
        if (clearTexture[0]) {
            viewer->disconnectTexture(1, status[1] == ViewerInstance::eViewerRenderRetCodeFail);
        }

        if ( ( (status[0] == ViewerInstance::eViewerRenderRetCodeRedraw) && !args[0]->params->isViewerPaused ) &&
                    ( ( status[1] == ViewerInstance::eViewerRenderRetCodeRedraw) && !args[1]->params->isViewerPaused ) ) {
            return;
        } else {
            for (int i = 0; i < 2; ++i) {
                if (args[i] && args[i]->params) {
                    if ( ( (args[i]->params->nbCachedTile > 0) && ( args[i]->params->nbCachedTile == (int)args[i]->params->tiles.size() ) ) || args[i]->params->isViewerPaused ) {
                        toAppend.push_back(args[i]->params);
                        args[i].reset();
                    }
                }
            }
        }


        if ( ( args[0] && (status[0] != ViewerInstance::eViewerRenderRetCodeFail) ) || ( args[1] && (status[1] != ViewerInstance::eViewerRenderRetCodeFail) ) ) {
            try {
                stat = viewer->renderViewer(view, false /*singleThreaded*/, true /*sequential*/, RotoStrokeItemPtr(), args, boost::shared_ptr<ViewerCurrentFrameRequestSchedulerStartArgs>(), stats);
            } catch (...) {
                stat = ViewerInstance::eViewerRenderRetCodeFail;
            }
        }
        if (stat == ViewerInstance::eViewerRenderRetCodeFail) {
            ///Don't report any error message otherwise we will flood the viewer with irrelevant messages such as
            ///"Render failed", instead we let the plug-in that failed post an error message which will be more helpful.
            _imp->scheduler->notifyRenderFailure( std::string() );
        } else {
            for (int i = 0; i < 2; ++i) {
                if (args[i] && args[i]->params) {
                    toAppend.push_back(args[i]->params);
                    args[i].reset();
                }
            }
        }
        _imp->scheduler->appendToBuffer(time, view, stats, toAppend);
    } // renderFrame
};



RenderThreadTask*
ViewerDisplayScheduler::createRunnable(TimeValue frame,
                                       bool useRenderStarts,
                                       const std::vector<ViewIdx>& viewsToRender)
{
    return new ViewerRenderFrameRunnable(_viewer.lock(), this, frame, useRenderStarts, viewsToRender);
}



void
ViewerDisplayScheduler::handleRenderFailure(const std::string& /*errorMessage*/)
{
    _viewer.lock()->disconnectViewer();
}

void
ViewerDisplayScheduler::onRenderStopped(bool /*/aborted*/)
{
    ///Refresh all previews in the tree
    boost::shared_ptr<ViewerInstance> viewer = _viewer.lock();

    viewer->getApp()->refreshAllPreviews();

    if ( !viewer->getApp() || viewer->getApp()->isGuiFrozen() ) {
        getEngine()->s_refreshAllKnobs();
    }
}

TimeValue
ViewerDisplayScheduler::getLastRenderedTime() const
{
    return TimeValue(_viewer.lock()->getLastRenderedTime());
}

////////////////////////// RenderEngine

struct RenderEnginePrivate
{
    QMutex schedulerCreationLock;
    OutputSchedulerThread* scheduler;

    //If true then a current frame render can start playback, protected by abortedRequestedMutex
    bool canAutoRestartPlayback;
    QMutex canAutoRestartPlaybackMutex; // protects abortRequested
    boost::weak_ptr<OutputEffectInstance> output;
    mutable QMutex pbModeMutex;
    PlaybackModeEnum pbMode;
    ViewerCurrentFrameRequestScheduler* currentFrameScheduler;

    // Only used on the main-thread
    boost::scoped_ptr<RenderEngineWatcher> engineWatcher;
    struct RefreshRequest
    {
        bool enableStats;
        bool enableAbort;
    };

    /*
       This queue tracks all calls made to renderCurrentFrame() and attempts to concatenate the calls
       once the event loop fires the signal currentFrameRenderRequestPosted()
       This is only accessed on the main thread
     */
    std::list<RefreshRequest> refreshQueue;

    RenderEnginePrivate(const OutputEffectInstancePtr& output)
        : schedulerCreationLock()
        , scheduler(0)
        , canAutoRestartPlayback(false)
        , canAutoRestartPlaybackMutex()
        , output(output)
        , pbModeMutex()
        , pbMode(ePlaybackModeLoop)
        , currentFrameScheduler(0)
        , refreshQueue()
    {
    }
};

RenderEngine::RenderEngine(const OutputEffectInstancePtr& output)
    : _imp( new RenderEnginePrivate(output) )
{
    QObject::connect(this, SIGNAL(currentFrameRenderRequestPosted()), this, SLOT(onCurrentFrameRenderRequestPosted()), Qt::QueuedConnection);
}

RenderEngine::~RenderEngine()
{
    delete _imp->currentFrameScheduler;
    _imp->currentFrameScheduler = 0;
    delete _imp->scheduler;
    _imp->scheduler = 0;
}

OutputSchedulerThread*
RenderEngine::createScheduler(const OutputEffectInstancePtr& effect)
{
    return new DefaultScheduler(this, effect);
}

OutputEffectInstancePtr
RenderEngine::getOutput() const
{
    return _imp->output.lock();
}

void
RenderEngine::renderFrameRange(bool isBlocking,
                               bool enableRenderStats,
                               TimeValue firstFrame,
                               TimeValue lastFrame,
                               TimeValue frameStep,
                               const std::vector<ViewIdx>& viewsToRender,
                               RenderDirectionEnum forward)
{
    // We are going to start playback, abort any current viewer refresh
    _imp->currentFrameScheduler->abortThreadedTask();
    
    setPlaybackAutoRestartEnabled(true);

    {
        QMutexLocker k(&_imp->schedulerCreationLock);
        if (!_imp->scheduler) {
            _imp->scheduler = createScheduler( _imp->output.lock() );
        }
    }

    _imp->scheduler->renderFrameRange(isBlocking, enableRenderStats, firstFrame, lastFrame, frameStep, viewsToRender, forward);
}

void
RenderEngine::renderFromCurrentFrame(bool enableRenderStats,
                                     const std::vector<ViewIdx>& viewsToRender,
                                     RenderDirectionEnum forward)
{
    // We are going to start playback, abort any current viewer refresh
    _imp->currentFrameScheduler->abortThreadedTask();
    
    setPlaybackAutoRestartEnabled(true);

    {
        QMutexLocker k(&_imp->schedulerCreationLock);
        if (!_imp->scheduler) {
            _imp->scheduler = createScheduler( _imp->output.lock() );
        }
    }

    _imp->scheduler->renderFromCurrentFrame(enableRenderStats, viewsToRender, forward);
}

void
RenderEngine::onCurrentFrameRenderRequestPosted()
{
    assert( QThread::currentThread() == qApp->thread() );

    //Okay we are at the end of the event loop, concatenate all similar events
    RenderEnginePrivate::RefreshRequest r;
    bool rSet = false;
    while ( !_imp->refreshQueue.empty() ) {
        const RenderEnginePrivate::RefreshRequest& queueBegin = _imp->refreshQueue.front();
        if (!rSet) {
            rSet = true;
        } else {
            if ( (queueBegin.enableAbort == r.enableAbort) && (queueBegin.enableStats == r.enableStats) ) {
                _imp->refreshQueue.pop_front();
                continue;
            }
        }
        r = queueBegin;
        renderCurrentFrameNow(r.enableStats, r.enableAbort);
        _imp->refreshQueue.pop_front();
    }
}

void
RenderEngine::renderCurrentFrameNow(bool enableRenderStats,
                                         bool canAbort)
{
    assert( QThread::currentThread() == qApp->thread() );

    ViewerInstancePtr isViewer = toViewerInstance( _imp->output.lock() );
    if (!isViewer) {
        qDebug() << "RenderEngine::renderCurrentFrame for a writer is unsupported";

        return;
    }


    ///If the scheduler is already doing playback, continue it
    if (_imp->scheduler) {
        bool working = _imp->scheduler->isWorking();
        if (working) {
            _imp->scheduler->abortThreadedTask();
        }
        if ( working || isPlaybackAutoRestartEnabled() ) {
            RenderDirectionEnum lastDirection;
            std::vector<ViewIdx> lastViews;
            _imp->scheduler->getLastRunArgs(&lastDirection, &lastViews);
            _imp->scheduler->renderFromCurrentFrame( enableRenderStats, lastViews,  lastDirection);

            return;
        }
    }


    {
        QMutexLocker k(&_imp->schedulerCreationLock);
        if (!_imp->scheduler) {
            _imp->scheduler = createScheduler( _imp->output.lock() );
        }
    }

    if (!_imp->currentFrameScheduler) {
        _imp->currentFrameScheduler = new ViewerCurrentFrameRequestScheduler(isViewer);
    }

    _imp->currentFrameScheduler->renderCurrentFrame(enableRenderStats, canAbort);
}

void
RenderEngine::renderCurrentFrame(bool enableRenderStats,
                                 bool canAbort)
{
    assert( QThread::currentThread() == qApp->thread() );
    RenderEnginePrivate::RefreshRequest r;
    r.enableStats = enableRenderStats;
    r.enableAbort = canAbort;
    _imp->refreshQueue.push_back(r);
    Q_EMIT currentFrameRenderRequestPosted();
}

void
RenderEngine::setPlaybackAutoRestartEnabled(bool enabled)
{
    QMutexLocker k(&_imp->canAutoRestartPlaybackMutex);

    _imp->canAutoRestartPlayback = enabled;
}

bool
RenderEngine::isPlaybackAutoRestartEnabled() const
{
    QMutexLocker k(&_imp->canAutoRestartPlaybackMutex);

    return _imp->canAutoRestartPlayback;
}

void
RenderEngine::quitEngine(bool allowRestarts)
{
    if (_imp->scheduler) {
        _imp->scheduler->quitThread(allowRestarts);
    }

    if (_imp->currentFrameScheduler) {
        _imp->currentFrameScheduler->quitThread(allowRestarts);
    }
}

void
RenderEngine::waitForEngineToQuit_not_main_thread()
{
    if (_imp->scheduler) {
        _imp->scheduler->waitForThreadToQuit_not_main_thread();
    }

    if (_imp->currentFrameScheduler) {
        _imp->currentFrameScheduler->waitForThreadToQuit_not_main_thread();
    }
}

void
RenderEngine::waitForEngineToQuit_main_thread(bool allowRestart)
{
    assert( QThread::currentThread() == qApp->thread() );
    assert(!_imp->engineWatcher);
    _imp->engineWatcher.reset( new RenderEngineWatcher(this) );
    QObject::connect( _imp->engineWatcher.get(), SIGNAL(taskFinished(int,WatcherCallerArgsPtr)), this, SLOT(onWatcherEngineQuitEmitted()) );
    _imp->engineWatcher->scheduleBlockingTask(allowRestart ? RenderEngineWatcher::eBlockingTaskWaitForQuitAllowRestart : RenderEngineWatcher::eBlockingTaskWaitForQuitDisallowRestart);
}

void
RenderEngine::waitForEngineToQuit_enforce_blocking()
{
    if (_imp->scheduler) {
        _imp->scheduler->waitForThreadToQuit_enforce_blocking();
    }

    if (_imp->currentFrameScheduler) {
        _imp->currentFrameScheduler->waitForThreadToQuit_enforce_blocking();
    }
}

bool
RenderEngine::abortRenderingInternal(bool keepOldestRender)
{
    bool ret = false;

    if (_imp->currentFrameScheduler) {
        ret |= _imp->currentFrameScheduler->abortThreadedTask(keepOldestRender);
    }

    if ( _imp->scheduler && _imp->scheduler->isWorking() ) {
        //If any playback active, abort it
        ret |= _imp->scheduler->abortThreadedTask(keepOldestRender);
    }

    return ret;
}

bool
RenderEngine::abortRenderingNoRestart(bool keepOldestRender)
{
    if ( abortRenderingInternal(keepOldestRender) ) {
        setPlaybackAutoRestartEnabled(false);

        return true;
    }

    return false;
}

bool
RenderEngine::abortRenderingAutoRestart()
{
    if ( abortRenderingInternal(true) ) {
        return true;
    }

    return false;
}

void
RenderEngine::waitForAbortToComplete_not_main_thread()
{
    if (_imp->currentFrameScheduler) {
        _imp->currentFrameScheduler->waitForAbortToComplete_not_main_thread();
    }
    if (_imp->scheduler) {
        _imp->scheduler->waitForAbortToComplete_not_main_thread();
    }
}

void
RenderEngine::waitForAbortToComplete_enforce_blocking()
{
    if (_imp->scheduler) {
        _imp->scheduler->waitForAbortToComplete_enforce_blocking();
    }

    if (_imp->currentFrameScheduler) {
        _imp->currentFrameScheduler->waitForAbortToComplete_enforce_blocking();
    }
}

void
RenderEngine::onWatcherEngineAbortedEmitted()
{
    assert(_imp->engineWatcher);
    if (!_imp->engineWatcher) {
        return;
    }
    _imp->engineWatcher.reset();
    Q_EMIT engineAborted();
}

void
RenderEngine::onWatcherEngineQuitEmitted()
{
    assert(_imp->engineWatcher);
    if (!_imp->engineWatcher) {
        return;
    }
    _imp->engineWatcher.reset();
    Q_EMIT engineQuit();
}

void
RenderEngine::waitForAbortToComplete_main_thread()
{
    assert( QThread::currentThread() == qApp->thread() );
    assert(!_imp->engineWatcher);
    _imp->engineWatcher.reset( new RenderEngineWatcher(this) );
    QObject::connect( _imp->engineWatcher.get(), SIGNAL(taskFinished(int,WatcherCallerArgsPtr)), this, SLOT(onWatcherEngineAbortedEmitted()) );
    _imp->engineWatcher->scheduleBlockingTask(RenderEngineWatcher::eBlockingTaskWaitForAbort);
}

bool
RenderEngine::isSequentialRenderBeingAborted() const
{
    if (!_imp->scheduler) {
        return false;
    }

    return _imp->scheduler->isBeingAborted();
}

bool
RenderEngine::hasThreadsAlive() const
{
    bool schedulerRunning = false;

    if (_imp->scheduler) {
        schedulerRunning = _imp->scheduler->isRunning();
    }
    bool currentFrameSchedulerRunning = false;
    if (_imp->currentFrameScheduler) {
        currentFrameSchedulerRunning = _imp->currentFrameScheduler->isRunning();
    }

    return schedulerRunning || currentFrameSchedulerRunning;
}

bool
RenderEngine::hasThreadsWorking() const
{
    bool working = false;

    if (_imp->scheduler) {
        working |= _imp->scheduler->isWorking();
    }

    if (!working && _imp->currentFrameScheduler) {
        working |= _imp->currentFrameScheduler->isWorking();
    }

    return working;
}

bool
RenderEngine::isDoingSequentialRender() const
{
    return _imp->scheduler ? _imp->scheduler->isWorking() : false;
}

void
RenderEngine::setPlaybackMode(int mode)
{
    QMutexLocker l(&_imp->pbModeMutex);

    _imp->pbMode = (PlaybackModeEnum)mode;
}

PlaybackModeEnum
RenderEngine::getPlaybackMode() const
{
    QMutexLocker l(&_imp->pbModeMutex);

    return _imp->pbMode;
}

void
RenderEngine::setDesiredFPS(double d)
{
    {
        QMutexLocker k(&_imp->schedulerCreationLock);
        if (!_imp->scheduler) {
            _imp->scheduler = createScheduler( _imp->output.lock() );
        }
    }
    _imp->scheduler->setDesiredFPS(d);
}

double
RenderEngine::getDesiredFPS() const
{
    return _imp->scheduler ? _imp->scheduler->getDesiredFPS() : 24;
}

void
RenderEngine::notifyFrameProduced(const BufferableObjectList& frames,
                                  const RenderStatsPtr& stats,
                                  const boost::shared_ptr<ViewerCurrentFrameRequestSchedulerStartArgs>& request)
{
    _imp->currentFrameScheduler->notifyFrameProduced(frames, stats, request);
}

OutputSchedulerThread*
ViewerRenderEngine::createScheduler(const OutputEffectInstancePtr& effect)
{
    return new ViewerDisplayScheduler( this, toViewerInstance(effect) );
}

////////////////////////ViewerCurrentFrameRequestScheduler////////////////////////
class CurrentFrameFunctorArgs
    : public GenericThreadStartArgs
{
public:

    ViewIdx view;
    int time;
    RenderStatsPtr stats;
    ViewerInstancePtr viewer;
    boost::shared_ptr<ViewerCurrentFrameRequestSchedulerStartArgs> request;
    ViewerCurrentFrameRequestSchedulerPrivate* scheduler;
    RotoStrokeItemPtr strokeItem;
    boost::shared_ptr<ViewerArgs> args[2];

    CurrentFrameFunctorArgs()
        : GenericThreadStartArgs()
        , view(0)
        , time(0)
        , stats()
        , viewer()
        , request()
        , scheduler(0)
        , strokeItem()
        , args()
    {
    }

    CurrentFrameFunctorArgs(ViewIdx view,
                            int time,
                            const RenderStatsPtr& stats,
                            const ViewerInstancePtr& viewer,
                            ViewerCurrentFrameRequestSchedulerPrivate* scheduler,
                            const RotoStrokeItemPtr& strokeItem)
        : GenericThreadStartArgs()
        , view(view)
        , time(time)
        , stats(stats)
        , viewer(viewer)
        , request()
        , scheduler(scheduler)
        , strokeItem(strokeItem)
        , args()
    {

    }

    ~CurrentFrameFunctorArgs()
    {

    }
};

class RenderCurrentFrameFunctorRunnable;
struct ViewerCurrentFrameRequestSchedulerPrivate
{
    ViewerInstancePtr viewer;
    QThreadPool* threadPool;
    QMutex producedFramesMutex;
    ProducedFrameSet producedFrames;
    QWaitCondition producedFramesNotEmpty;


    /**
     * Single thread used by the ViewerCurrentFrameRequestScheduler when the global thread pool has reached its maximum
     * activity to keep the renders responsive even if the thread pool is choking.
     **/
    ViewerCurrentFrameRequestRendererBackup backupThread;
    mutable QMutex currentFrameRenderTasksMutex;
    QWaitCondition currentFrameRenderTasksCond;
    std::list<RenderCurrentFrameFunctorRunnable*> currentFrameRenderTasks;

    // Used to attribute an age to each renderCurrentFrameRequest
    U64 ageCounter;

    ViewerCurrentFrameRequestSchedulerPrivate(const ViewerInstancePtr& viewer)
        : viewer(viewer)
        , threadPool( QThreadPool::globalInstance() )
        , producedFramesMutex()
        , producedFrames()
        , producedFramesNotEmpty()
        , backupThread()
        , currentFrameRenderTasksCond()
        , currentFrameRenderTasks()
        , ageCounter(0)
    {
    }

    void appendRunnableTask(RenderCurrentFrameFunctorRunnable* task)
    {
        {
            QMutexLocker k(&currentFrameRenderTasksMutex);
            currentFrameRenderTasks.push_back(task);
        }
    }

    void removeRunnableTask(RenderCurrentFrameFunctorRunnable* task)
    {
        {
            QMutexLocker k(&currentFrameRenderTasksMutex);
            for (std::list<RenderCurrentFrameFunctorRunnable*>::iterator it = currentFrameRenderTasks.begin();
                 it != currentFrameRenderTasks.end(); ++it) {
                if (*it == task) {
                    currentFrameRenderTasks.erase(it);

                    currentFrameRenderTasksCond.wakeAll();
                    break;
                }
            }
        }
    }

    void waitForRunnableTasks()
    {
        QMutexLocker k(&currentFrameRenderTasksMutex);

        while ( !currentFrameRenderTasks.empty() ) {
            currentFrameRenderTasksCond.wait(&currentFrameRenderTasksMutex);
        }
    }

    void notifyFrameProduced(const BufferableObjectList& frames,
                             const RenderStatsPtr& stats,
                             U64 age)
    {
        QMutexLocker k(&producedFramesMutex);
        ProducedFrame p;

        p.frames = frames;
        p.age = age;
        p.stats = stats;
        producedFrames.insert(p);
        producedFramesNotEmpty.wakeOne();
    }

    void processProducedFrame(const RenderStatsPtr& stats, const BufferableObjectList& frames);
};

class RenderCurrentFrameFunctorRunnable
    : public QRunnable
{
    boost::shared_ptr<CurrentFrameFunctorArgs> _args;

public:

    RenderCurrentFrameFunctorRunnable(const boost::shared_ptr<CurrentFrameFunctorArgs>& args)
        : _args(args)
    {
    }

    virtual ~RenderCurrentFrameFunctorRunnable()
    {
    }

    virtual void run() OVERRIDE FINAL
    {
        ///The viewer always uses the scheduler thread to regulate the output rate, @see ViewerInstance::renderViewer_internal
        ///it calls appendToBuffer by itself
        ViewerInstance::ViewerRenderRetCode stat = ViewerInstance::eViewerRenderRetCodeFail;
        BufferableObjectList ret;
        try {
            stat = _args->viewer->renderViewer(_args->view, QThread::currentThread() == qApp->thread(), false,
                                               _args->strokeItem, _args->args, _args->request, _args->stats);
        } catch (...) {
            stat = ViewerInstance::eViewerRenderRetCodeFail;
        }

        if (stat == ViewerInstance::eViewerRenderRetCodeFail) {
            ///Don't report any error message otherwise we will flood the viewer with irrelevant messages such as
            ///"Render failed", instead we let the plug-in that failed post an error message which will be more helpful.
            _args->viewer->disconnectViewer();
        } else {
            for (int i = 0; i < 2; ++i) {
                if (_args->args[i] && _args->args[i]->params) {
                    if (_args->args[i]->params->tiles.size() > 0) {
                        ret.push_back(_args->args[i]->params);
                    }
                }
            }
        }

        if (_args->request) {
#ifdef DEBUG
            for (BufferableObjectList::iterator it = ret.begin(); it != ret.end(); ++it) {
                UpdateViewerParamsPtr isParams = boost::dynamic_pointer_cast<UpdateViewerParams>(*it);
                assert(isParams);
                assert( !isParams->tiles.empty() );
                for (std::list<UpdateViewerParams::CachedTile>::iterator it2 = isParams->tiles.begin(); it2 != isParams->tiles.end(); ++it2) {
                    assert(it2->ramBuffer);
                }
            }
#endif
            _args->scheduler->notifyFrameProduced(ret, _args->stats, _args->request->age);
        } else {
            assert( QThread::currentThread() == qApp->thread() );
            _args->scheduler->processProducedFrame(_args->stats, ret);
        }

        _args->request.reset();
        _args->args[0].reset();
        _args->args[1].reset();

        ///This thread is done, clean-up its TLS
        appPTR->getAppTLS()->cleanupTLSForThread();


        _args->scheduler->removeRunnableTask(this);
    } // run
};


class ViewerCurrentFrameRequestSchedulerExecOnMT
    : public GenericThreadExecOnMainThreadArgs
{
public:

    RenderStatsPtr stats;
    BufferableObjectList frames;

    ViewerCurrentFrameRequestSchedulerExecOnMT()
        : GenericThreadExecOnMainThreadArgs()
    {
    }

    virtual ~ViewerCurrentFrameRequestSchedulerExecOnMT()
    {
    }
};

ViewerCurrentFrameRequestScheduler::ViewerCurrentFrameRequestScheduler(const ViewerInstancePtr& viewer)
    : GenericSchedulerThread()
    , _imp( new ViewerCurrentFrameRequestSchedulerPrivate(viewer) )
{
    setThreadName("ViewerCurrentFrameRequestScheduler");
}

ViewerCurrentFrameRequestScheduler::~ViewerCurrentFrameRequestScheduler()
{
    // Should've been stopped before anyway
    if (_imp->backupThread.quitThread(false)) {
        _imp->backupThread.waitForAbortToComplete_enforce_blocking();
    }
}

GenericSchedulerThread::TaskQueueBehaviorEnum
ViewerCurrentFrameRequestScheduler::tasksQueueBehaviour() const
{
    return eTaskQueueBehaviorSkipToMostRecent;
}

GenericSchedulerThread::ThreadStateEnum
ViewerCurrentFrameRequestScheduler::threadLoopOnce(const ThreadStartArgsPtr &inArgs)
{
    ThreadStateEnum state = eThreadStateActive;
    boost::shared_ptr<ViewerCurrentFrameRequestSchedulerStartArgs> args = boost::dynamic_pointer_cast<ViewerCurrentFrameRequestSchedulerStartArgs>(inArgs);
    assert(args);

#ifdef TRACE_CURRENT_FRAME_SCHEDULER
    qDebug() << getThreadName().c_str() << "Thread loop once, starting" << args->age ;
#endif


    // Start the work in a thread of the thread pool if we can.
    // Let at least 1 free thread in the thread-pool to allow the renderer to use the thread pool if we use the thread-pool
    int maxThreads = _imp->threadPool->maxThreadCount();
    if (args->useSingleThread) {
        maxThreads = 1;
    }
    args->functorArgs->request = args;
    if ( (maxThreads == 1) || (_imp->threadPool->activeThreadCount() >= maxThreads - 1) ) {
        _imp->backupThread.startTask(args->functorArgs);
    } else {
        RenderCurrentFrameFunctorRunnable* task = new RenderCurrentFrameFunctorRunnable(args->functorArgs);
        _imp->appendRunnableTask(task);
        _imp->threadPool->start(task);
    }

    // Clear the shared ptr now that we started the task in the thread pool
    args->functorArgs.reset();

    // Wait for the work to be done
    boost::shared_ptr<ViewerCurrentFrameRequestSchedulerExecOnMT> mtArgs(new ViewerCurrentFrameRequestSchedulerExecOnMT);
    {
        QMutexLocker k(&_imp->producedFramesMutex);
        ProducedFrameSet::iterator found = _imp->producedFrames.end();
        for (ProducedFrameSet::iterator it = _imp->producedFrames.begin(); it != _imp->producedFrames.end(); ++it) {
            if (it->age == args->age) {
                found = it;
                break;
            }
        }

        while ( found == _imp->producedFrames.end() ) {
            state = resolveState();
            if ( (state == eThreadStateStopped) || (state == eThreadStateAborted) ) {
                //_imp->producedFrames.clear();
                break;
            }
            // Wait at most 100ms and re-check, so that we can resolveState() again:
            // Imagine we launched 1 render (very long) that is not being aborted (the viewer always keeps 1 thread running)
            // then this thread would be stuck here and would never launch a new render.
            _imp->producedFramesNotEmpty.wait(&_imp->producedFramesMutex, 100);
            for (ProducedFrameSet::iterator it = _imp->producedFrames.begin(); it != _imp->producedFrames.end(); ++it) {
                if (it->age == args->age) {
                    found = it;
                    break;
                }
            }
        }
        if ( found != _imp->producedFrames.end() ) {
#ifdef TRACE_CURRENT_FRAME_SCHEDULER
            qDebug() << getThreadName().c_str() << "Found" << args->age << "produced";
#endif

            mtArgs->frames = found->frames;
            mtArgs->stats = found->stats;

            // Erase from the produced frames all renders that are older that the age we want to render
            // since they are no longer going to be used.
            // Only do this if the render had frames it is not just a redraw
            //if (!found->frames.empty()) {
            //    ++found;
            //    _imp->producedFrames.erase(_imp->producedFrames.begin(), found);
            //} else {
            ++found;
            _imp->producedFrames.erase(_imp->producedFrames.begin(), found);
            //}
        } else {
#ifdef TRACE_CURRENT_FRAME_SCHEDULER
            qDebug() << getThreadName().c_str() << "Got aborted, skip waiting for" << args->age;
#endif
        }
    } // QMutexLocker k(&_imp->producedQueueMutex);

    if (state == eThreadStateActive) {
        state = resolveState();
    }
    // Do not uncomment the second part: if we don't update on the viewer aborted tasks then user will never even see valid images that were fully rendered
    if (state == eThreadStateStopped /*|| (state == eThreadStateAborted)*/ ) {
        return state;
    }

    requestExecutionOnMainThread(mtArgs);

#ifdef TRACE_CURRENT_FRAME_SCHEDULER
    qDebug() << getThreadName().c_str() << "Frame processed" << args->age;
#endif

    return state;
} // ViewerCurrentFrameRequestScheduler::threadLoopOnce

void
ViewerCurrentFrameRequestScheduler::executeOnMainThread(const ExecOnMTArgsPtr& inArgs)
{
    ViewerCurrentFrameRequestSchedulerExecOnMT* args = dynamic_cast<ViewerCurrentFrameRequestSchedulerExecOnMT*>( inArgs.get() );

    assert(args);
    if (args) {
        _imp->processProducedFrame(args->stats, args->frames);
    }
}

void
ViewerCurrentFrameRequestSchedulerPrivate::processProducedFrame(const RenderStatsPtr& stats,
                                                                const BufferableObjectList& frames)
{
    assert( QThread::currentThread() == qApp->thread() );

    if ( !frames.empty() ) {
        viewer->aboutToUpdateTextures();
    }

    //bool hasDoneSomething = false;
    for (BufferableObjectList::const_iterator it2 = frames.begin(); it2 != frames.end(); ++it2) {
        assert(*it2);
        boost::shared_ptr<UpdateViewerParams> params = boost::dynamic_pointer_cast<UpdateViewerParams>(*it2);
        assert(params);
        if ( params && (params->tiles.size() >= 1) ) {
            if (stats) {
                double timeSpent;
                std::map<NodePtr, NodeRenderStats > ret = stats->getStats(&timeSpent);
                viewer->reportStats(0, ViewIdx(0), timeSpent, ret);
            }

            viewer->updateViewer(params);
        }
    }


    ///At least redraw the viewer, we might be here when the user removed a node upstream of the viewer.
    viewer->redrawViewer();
}

void
ViewerCurrentFrameRequestScheduler::onAbortRequested(bool keepOldestRender)
{
#ifdef TRACE_CURRENT_FRAME_SCHEDULER
    qDebug() << getThreadName().c_str() << "Received abort request";
#endif
    //This will make all processing nodes that call the abort() function return true
    //This function marks all active renders of the viewer as aborted (except the oldest one)
    //and each node actually check if the render has been aborted in EffectInstance::Implementation::aborted()
    _imp->viewer->markAllOnGoingRendersAsAborted(keepOldestRender);
    _imp->backupThread.abortThreadedTask();
}

void
ViewerCurrentFrameRequestScheduler::onQuitRequested(bool allowRestarts)
{
    _imp->backupThread.quitThread(allowRestarts);
}

void
ViewerCurrentFrameRequestScheduler::onWaitForThreadToQuit()
{
    _imp->waitForRunnableTasks();
    _imp->backupThread.waitForThreadToQuit_enforce_blocking();
}

void
ViewerCurrentFrameRequestScheduler::onWaitForAbortCompleted()
{
    _imp->waitForRunnableTasks();
    _imp->backupThread.waitForAbortToComplete_enforce_blocking();
}

void
ViewerCurrentFrameRequestScheduler::notifyFrameProduced(const BufferableObjectList& frames,
                                                        const RenderStatsPtr& stats,
                                                        const boost::shared_ptr<ViewerCurrentFrameRequestSchedulerStartArgs>& request)
{
    _imp->notifyFrameProduced(frames, stats,  request->age);
}

void
ViewerCurrentFrameRequestScheduler::renderCurrentFrame(bool enableRenderStats,
                                                       bool canAbort)
{
    // Sanity check, also do not render viewer that are not made visible by the user
    if (!_imp->viewer || !_imp->viewer->getNode() || !_imp->viewer->isViewerUIVisible() ) {
        return;
    }

    // Get the frame/view to render
    TimeValue frame;
    ViewIdx view;
    {
        frame = TimeValue(_imp->viewer->getTimeline()->currentFrame());
        int viewsCount = _imp->viewer->getRenderViewsCount();
        view = viewsCount > 0 ? _imp->viewer->getCurrentView() : ViewIdx(0);
    }

    // Init ret code
    ViewerInstance::ViewerRenderRetCode status[2] = {
        ViewerInstance::eViewerRenderRetCodeFail, ViewerInstance::eViewerRenderRetCodeFail
    };


    // Create stats object if we want statistics
    RenderStatsPtr stats;
    if (enableRenderStats) {
        stats.reset( new RenderStats(enableRenderStats) );
    }

    // Are we tracking ?
    bool isTracking = _imp->viewer->isDoingPartialUpdates();

    // Are we painting ?
    RotoStrokeItemPtr curStroke = _imp->viewer->getApp()->getActiveRotoDrawingStroke();

    // While drawing, use a single render thread and always the same thread.
    bool rotoUse1Thread = false;
    if (curStroke) {
        rotoUse1Thread = true;
    }

    // For each A and B inputs, the viewer render args
    boost::shared_ptr<ViewerArgs> args[2];

    // Check if we need to clear viewer to black
    bool clearTexture[2] = {false, false};

    // For each input get the render args
    for (int i = 0; i < 2; ++i) {
        args[i].reset(new ViewerArgs);
        status[i] = _imp->viewer->getRenderViewerArgsAndCheckCache_public( frame, false /*sequential*/, view, i /*input (A or B)*/, canAbort, curStroke, stats, args[i].get() );

        clearTexture[i] = status[i] == ViewerInstance::eViewerRenderRetCodeFail || status[i] == ViewerInstance::eViewerRenderRetCodeBlack;

        // Fail black or paused, reset the params so that we don't launch the render thread
        if (clearTexture[i] || args[i]->params->isViewerPaused) {
            //Just clear the viewer, nothing to do
            args[i]->params.reset();
        }

        if ( (status[i] == ViewerInstance::eViewerRenderRetCodeRedraw) && args[i]->params ) {
            //We must redraw (re-render) don't hold a pointer to the cached frame
            args[i]->params->tiles.clear();
        }
    }

    // Both inputs are failed ? Clear viewer
    if (status[0] == ViewerInstance::eViewerRenderRetCodeFail && status[1] == ViewerInstance::eViewerRenderRetCodeFail) {
        _imp->viewer->disconnectViewer();
        return;
    }

    // Clear each input individually
    if (clearTexture[0]) {
        _imp->viewer->disconnectTexture(0, status[0] == ViewerInstance::eViewerRenderRetCodeFail);
    }
    if (clearTexture[0]) {
        _imp->viewer->disconnectTexture(1, status[1] == ViewerInstance::eViewerRenderRetCodeFail);
    }

    // If both just needs a redraw don't render
    if ( (status[0] == ViewerInstance::eViewerRenderRetCodeRedraw) && (status[1] == ViewerInstance::eViewerRenderRetCodeRedraw) ) {
        _imp->viewer->redrawViewer();
        return;
    }

    // Report cached frames in the viewer
    bool hasTextureCached = false;
    for (int i = 0; i < 2; ++i) {
        if ( args[i]->params && (args[i]->params->nbCachedTile > 0) && ( args[i]->params->nbCachedTile == (int)args[i]->params->tiles.size() ) ) {
            hasTextureCached = true;
            break;
        }
    }

    // If any frame is cached it means we are about to display a new texture
    if (hasTextureCached) {
        _imp->viewer->aboutToUpdateTextures();
    }

    for (int i = 0; i < 2; ++i) {
        if ( args[i]->params && (args[i]->params->nbCachedTile > 0) && ( args[i]->params->nbCachedTile == (int)args[i]->params->tiles.size() ) ) {
            // The texture was cached
            // Report stats from input A only
            if ( stats && (i == 0) ) {
                double timeSpent;
                std::map<NodePtr, NodeRenderStats > statResults = stats->getStats(&timeSpent);
                _imp->viewer->reportStats(frame, view, timeSpent, statResults);
            }
            _imp->viewer->updateViewer(args[i]->params);
            args[i].reset();
        }
    }

    // Nothing left to render
    if ( (!args[0] && !args[1]) ||
        ( !args[0] && ( status[0] == ViewerInstance::eViewerRenderRetCodeRender) && args[1] && ( status[1] == ViewerInstance::eViewerRenderRetCodeFail) ) ||
        ( !args[1] && ( status[1] == ViewerInstance::eViewerRenderRetCodeRender) && args[0] && ( status[0] == ViewerInstance::eViewerRenderRetCodeFail) ) ) {
        _imp->viewer->redrawViewer();

        return;
    }


    // Ok we have to render at least one of A or B input
    boost::shared_ptr<CurrentFrameFunctorArgs> functorArgs( new CurrentFrameFunctorArgs(view,
                                                                                        frame,
                                                                                        stats,
                                                                                        _imp->viewer,
                                                                                        _imp.get(),
                                                                                        curStroke) );
    functorArgs->args[0] = args[0];
    functorArgs->args[1] = args[1];


    if (appPTR->getCurrentSettings()->getNumberOfThreads() == -1) {
        RenderCurrentFrameFunctorRunnable task(functorArgs);
        task.run();
    } else {
        // Identify this render request with an age
        boost::shared_ptr<ViewerCurrentFrameRequestSchedulerStartArgs> request(new ViewerCurrentFrameRequestSchedulerStartArgs);
        request->age = _imp->ageCounter;
        request->functorArgs = functorArgs;
        // When painting, limit the number of threads to 1 to be sure strokes are painted in the right order
        request->useSingleThread = rotoUse1Thread || isTracking;

        // If we reached the max amount of age, reset to 0... should never happen anyway
        if ( _imp->ageCounter >= std::numeric_limits<U64>::max() ) {
            _imp->ageCounter = 0;
        } else {
            ++_imp->ageCounter;
        }

        startTask(request);

    }
} // ViewerCurrentFrameRequestScheduler::renderCurrentFrame

ViewerCurrentFrameRequestRendererBackup::ViewerCurrentFrameRequestRendererBackup()
    : GenericSchedulerThread()
{
    setThreadName("ViewerCurrentFrameRequestRendererBackup");
}

ViewerCurrentFrameRequestRendererBackup::~ViewerCurrentFrameRequestRendererBackup()
{
}

GenericSchedulerThread::TaskQueueBehaviorEnum
ViewerCurrentFrameRequestRendererBackup::tasksQueueBehaviour() const 
{
    return eTaskQueueBehaviorSkipToMostRecent;
}


GenericSchedulerThread::ThreadStateEnum
ViewerCurrentFrameRequestRendererBackup::threadLoopOnce(const ThreadStartArgsPtr& inArgs)
{
    boost::shared_ptr<CurrentFrameFunctorArgs> args = boost::dynamic_pointer_cast<CurrentFrameFunctorArgs>(inArgs);

    assert(args);
    RenderCurrentFrameFunctorRunnable task(args);
    task.run();

    return eThreadStateActive;
}

NATRON_NAMESPACE_EXIT;

NATRON_NAMESPACE_USING;
#include "moc_OutputSchedulerThread.cpp"
