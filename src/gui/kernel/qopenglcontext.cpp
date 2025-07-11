// Copyright (C) 2016 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only

#include <qpa/qplatformopenglcontext.h>
#include <qpa/qplatformintegration.h>
#include "qopenglcontext.h"
#include "qopenglcontext_p.h"
#include "qwindow.h"

#include <QtCore/QThreadStorage>
#include <QtCore/QThread>
#include <QtCore/private/qlocking_p.h>

#include <QtGui/private/qguiapplication_p.h>
#include <QtGui/private/qopengl_p.h>
#include <QtGui/private/qwindow_p.h>
#include <QtGui/QScreen>
#include <qpa/qplatformnativeinterface.h>

#include <private/qopenglextensions_p.h>

#include <QDebug>

QT_BEGIN_NAMESPACE

class QGuiGLThreadContext
{
public:
    QGuiGLThreadContext()
        : context(nullptr)
    {
    }
    ~QGuiGLThreadContext() {
        if (context)
            context->doneCurrent();
    }
    QOpenGLContext *context;
};

Q_GLOBAL_STATIC(QThreadStorage<QGuiGLThreadContext *>, qwindow_context_storage);
static QOpenGLContext *global_share_context = nullptr;

#ifndef QT_NO_DEBUG
QHash<QOpenGLContext *, bool> QOpenGLContextPrivate::makeCurrentTracker;
Q_CONSTINIT QMutex QOpenGLContextPrivate::makeCurrentTrackerMutex;
#endif

/*!
    \internal

    This function is used by Qt::AA_ShareOpenGLContexts and the Qt
    WebEngine to set up context sharing across multiple windows. Do
    not use it for any other purpose.

    Please maintain the binary compatibility of these functions.
*/
void qt_gl_set_global_share_context(QOpenGLContext *context)
{
    global_share_context = context;
}

/*!
    \internal
*/
QOpenGLContext *qt_gl_global_share_context()
{
    return global_share_context;
}

/*!
    \class QOpenGLContext
    \ingroup painting-3D
    \inmodule QtGui
    \since 5.0
    \brief The QOpenGLContext class represents a native OpenGL context, enabling
           OpenGL rendering on a QSurface.

    QOpenGLContext represents the OpenGL state of an underlying OpenGL context.
    To set up a context, set its screen and format such that they match those
    of the surface or surfaces with which the context is meant to be used, if
    necessary make it share resources with other contexts with
    setShareContext(), and finally call create(). Use the return value or isValid()
    to check if the context was successfully initialized.

    A context can be made current against a given surface by calling
    makeCurrent(). When OpenGL rendering is done, call swapBuffers() to swap
    the front and back buffers of the surface, so that the newly rendered
    content becomes visible. To be able to support certain platforms,
    QOpenGLContext requires that you call makeCurrent() again before starting
    rendering a new frame, after calling swapBuffers().

    If the context is temporarily not needed, such as when the application is
    not rendering, it can be useful to delete it in order to free resources.
    You can connect to the aboutToBeDestroyed() signal to clean up any
    resources that have been allocated with different ownership from the
    QOpenGLContext itself.

    Once a QOpenGLContext has been made current, you can render to it in a
    platform independent way by using Qt's OpenGL enablers such as
    QOpenGLFunctions, QOpenGLBuffer, QOpenGLShaderProgram, and
    QOpenGLFramebufferObject. It is also possible to use the platform's OpenGL
    API directly, without using the Qt enablers, although potentially at the
    cost of portability. The latter is necessary when wanting to use OpenGL 1.x
    or OpenGL ES 1.x.

    For more information about the OpenGL API, refer to the official
    \l{http://www.opengl.org}{OpenGL documentation}.

    For an example of how to use QOpenGLContext see the
    \l{OpenGL Window Example}{OpenGL Window} example.

    \section1 Thread Affinity

    QOpenGLContext can be moved to a different thread with moveToThread(). Do
    not call makeCurrent() from a different thread than the one to which the
    QOpenGLContext object belongs. A context can only be current in one thread
    and against one surface at a time, and a thread only has one context
    current at a time.

    \section1 Context Resource Sharing

    Resources such as textures and vertex buffer objects
    can be shared between contexts.  Use setShareContext() before calling
    create() to specify that the contexts should share these resources.
    QOpenGLContext internally keeps track of a QOpenGLContextGroup object which
    can be accessed with shareGroup(), and which can be used to find all the
    contexts in a given share group. A share group consists of all contexts that
    have been successfully initialized and are sharing with an existing context in
    the share group. A non-sharing context has a share group consisting of a
    single context.

    \section1 Default Framebuffer

    On certain platforms, a framebuffer other than 0 might be the default frame
    buffer depending on the current surface. Instead of calling
    glBindFramebuffer(0), it is recommended that you use
    glBindFramebuffer(ctx->defaultFramebufferObject()), to ensure that your
    application is portable between different platforms. However, if you use
    QOpenGLFunctions::glBindFramebuffer(), this is done automatically for you.

    \warning WebAssembly

    We recommend that only one QOpenGLContext is made current with a QSurface,
    for the entire lifetime of the QSurface. Should more than once context be used,
    it is important to understand that multiple QOpenGLContext instances may be
    backed by the same native context underneath with the WebAssembly platform.
    Therefore, calling makeCurrent() with the same QSurface on two QOpenGLContext
    objects may not switch to a different native context in the second call. As
    a result, any OpenGL state changes done after the second makeCurrent() may
    alter the state of the first QOpenGLContext as well, as they are all backed
    by the same native context.

    \note This means that when targeting WebAssembly with existing OpenGL-based
    Qt code, some porting may be required to cater to these limitations.


    \sa QOpenGLFunctions, QOpenGLBuffer, QOpenGLShaderProgram, QOpenGLFramebufferObject
*/

/*!
    \internal

    Set the current context. Returns the previously current context.
*/
QOpenGLContext *QOpenGLContextPrivate::setCurrentContext(QOpenGLContext *context)
{
    QGuiGLThreadContext *threadContext = qwindow_context_storage()->localData();
    if (!threadContext) {
        if (!QThread::currentThread()) {
            qWarning("No QTLS available. currentContext won't work");
            return nullptr;
        }
        if (!context)
            return nullptr;

        threadContext = new QGuiGLThreadContext;
        qwindow_context_storage()->setLocalData(threadContext);
    }
    QOpenGLContext *previous = threadContext->context;
    threadContext->context = context;
    return previous;
}

int QOpenGLContextPrivate::maxTextureSize()
{
    if (max_texture_size != -1)
        return max_texture_size;

    Q_Q(QOpenGLContext);
    QOpenGLFunctions *funcs = q->functions();
    funcs->glGetIntegerv(GL_MAX_TEXTURE_SIZE, &max_texture_size);

#if !QT_CONFIG(opengles2)
    if (!q->isOpenGLES()) {
        GLenum proxy = GL_PROXY_TEXTURE_2D;

        GLint size;
        GLint next = 64;
        funcs->glTexImage2D(proxy, 0, GL_RGBA, next, next, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);

        QOpenGLExtraFunctions *extraFuncs = q->extraFunctions();
        extraFuncs->glGetTexLevelParameteriv(proxy, 0, GL_TEXTURE_WIDTH, &size);

        if (size == 0) {
            return max_texture_size;
        }
        do {
            size = next;
            next = size * 2;

            if (next > max_texture_size)
                break;
            funcs->glTexImage2D(proxy, 0, GL_RGBA, next, next, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
            extraFuncs->glGetTexLevelParameteriv(proxy, 0, GL_TEXTURE_WIDTH, &next);
        } while (next > size);

        max_texture_size = size;
    }
#endif // QT_CONFIG(opengles2)

    return max_texture_size;
}

/*!
    Returns the last context which called makeCurrent in the current thread,
    or \nullptr, if no context is current.
*/
QOpenGLContext* QOpenGLContext::currentContext()
{
    QGuiGLThreadContext *threadContext = qwindow_context_storage()->localData();
    if (threadContext)
        return threadContext->context;
    return nullptr;
}

/*!
    Returns \c true if the \a first and \a second contexts are sharing OpenGL resources.
*/
bool QOpenGLContext::areSharing(QOpenGLContext *first, QOpenGLContext *second)
{
    return first->shareGroup() == second->shareGroup();
}

/*!
    Returns the underlying platform context.

    \internal
*/
QPlatformOpenGLContext *QOpenGLContext::handle() const
{
    Q_D(const QOpenGLContext);
    return d->platformGLContext;
}

/*!
    Returns the underlying platform context with which this context is sharing.

    \internal
*/

QPlatformOpenGLContext *QOpenGLContext::shareHandle() const
{
    Q_D(const QOpenGLContext);
    if (d->shareContext)
        return d->shareContext->handle();
    return nullptr;
}

/*!
    Creates a new OpenGL context instance with parent object \a parent.

    Before it can be used you need to set the proper format and call create().

    \sa create(), makeCurrent()
*/
QOpenGLContext::QOpenGLContext(QObject *parent)
    : QObject(*new QOpenGLContextPrivate(), parent)
{
    setScreen(QGuiApplication::primaryScreen());
}

/*!
    Sets the \a format the OpenGL context should be compatible with. You need
    to call create() before it takes effect.

    When the format is not explicitly set via this function, the format returned
    by QSurfaceFormat::defaultFormat() will be used. This means that when having
    multiple contexts, individual calls to this function can be replaced by one
    single call to QSurfaceFormat::setDefaultFormat() before creating the first
    context.
*/
void QOpenGLContext::setFormat(const QSurfaceFormat &format)
{
    Q_D(QOpenGLContext);
    d->requestedFormat = format;
}

/*!
    Makes this context share textures, shaders, and other OpenGL resources
    with \a shareContext. You need to call create() before it takes effect.
*/
void QOpenGLContext::setShareContext(QOpenGLContext *shareContext)
{
    Q_D(QOpenGLContext);
    d->shareContext = shareContext;
}

/*!
    Sets the \a screen the OpenGL context should be valid for. You need to call
    create() before it takes effect.
*/
void QOpenGLContext::setScreen(QScreen *screen)
{
    Q_D(QOpenGLContext);
    if (d->screen)
        disconnect(d->screen, SIGNAL(destroyed(QObject*)), this, SLOT(_q_screenDestroyed(QObject*)));
    d->screen = screen;
    if (!d->screen)
        d->screen = QGuiApplication::primaryScreen();
    if (d->screen)
        connect(d->screen, SIGNAL(destroyed(QObject*)), this, SLOT(_q_screenDestroyed(QObject*)));
}

void QOpenGLContextPrivate::_q_screenDestroyed(QObject *object)
{
    Q_Q(QOpenGLContext);
    if (object == static_cast<QObject *>(screen)) {
        screen = nullptr;
        q->setScreen(nullptr);
    }
}

/*!
    \fn template <typename QNativeInterface> QNativeInterface *QOpenGLContext::nativeInterface() const

    Returns a native interface of the given type for the context.

    This function provides access to platform specific functionality
    of QOpenGLContext, as defined in the QNativeInterface namespace:

    \annotatedlist native-interfaces-qopenglcontext

    If the requested interface is not available a \nullptr is returned.
 */

/*!
    Attempts to create the OpenGL context with the current configuration.

    The current configuration includes the format, the share context, and the
    screen.

    If the OpenGL implementation on your system does not support the requested
    version of OpenGL context, then QOpenGLContext will try to create the closest
    matching version. The actual created context properties can be queried
    using the QSurfaceFormat returned by the format() function. For example, if
    you request a context that supports OpenGL 4.3 Core profile but the driver
    and/or hardware only supports version 3.2 Core profile contexts then you will
    get a 3.2 Core profile context.

    Returns \c true if the native context was successfully created and is ready to
    be used with makeCurrent(), swapBuffers(), etc.

    \note If the context already exists, this function destroys the existing
    context first, and then creates a new one.

    \sa makeCurrent(), format()
*/
bool QOpenGLContext::create()
{
    Q_D(QOpenGLContext);
    if (d->platformGLContext)
        destroy();

    auto *platformContext = QGuiApplicationPrivate::platformIntegration()->createPlatformOpenGLContext(this);
    if (!platformContext)
        return false;

    d->adopt(platformContext);

    return isValid();
}

QOpenGLContextPrivate::~QOpenGLContextPrivate()
{
}

void QOpenGLContextPrivate::adopt(QPlatformOpenGLContext *context)
{
    Q_Q(QOpenGLContext);

    platformGLContext = context;
    platformGLContext->setContext(q);
    platformGLContext->initialize();

    if (!platformGLContext->isSharing())
        shareContext = nullptr;
    shareGroup = shareContext ? shareContext->shareGroup() : new QOpenGLContextGroup;
    shareGroup->d_func()->addContext(q);
}

/*!
    \internal

    Destroy the underlying platform context associated with this context.

    If any other context is directly or indirectly sharing resources with this
    context, the shared resources, which includes vertex buffer objects, shader
    objects, textures, and framebuffer objects, are not freed. However,
    destroying the underlying platform context frees any state associated with
    the context.

    After \c destroy() has been called, you must call create() if you wish to
    use the context again.

    \note This implicitly calls doneCurrent() if the context is current.

    \sa create()
*/
void QOpenGLContext::destroy()
{
    Q_D(QOpenGLContext);

    // Notify that the native context and the QPlatformOpenGLContext are going
    // to go away.
    if (d->platformGLContext)
        emit aboutToBeDestroyed();

    // Invoke callbacks for helpers and invalidate.
    if (d->textureFunctionsDestroyCallback) {
        d->textureFunctionsDestroyCallback();
        d->textureFunctionsDestroyCallback = nullptr;
    }
    d->textureFunctions = nullptr;

    delete d->versionFunctions;
    d->versionFunctions = nullptr;

    if (d->vaoHelperDestroyCallback) {
        Q_ASSERT(d->vaoHelper);
        d->vaoHelperDestroyCallback(d->vaoHelper);
        d->vaoHelperDestroyCallback = nullptr;
    }
    d->vaoHelper = nullptr;

    // Tear down function wrappers.
    delete d->versionFunctions;
    d->versionFunctions = nullptr;

    delete d->functions;
    d->functions = nullptr;

    // Clean up and destroy the native context machinery.
    if (QOpenGLContext::currentContext() == this)
        doneCurrent();

    if (d->shareGroup)
        d->shareGroup->d_func()->removeContext(this);

    d->shareGroup = nullptr;

    delete d->platformGLContext;
    d->platformGLContext = nullptr;
}

/*!
    \fn void QOpenGLContext::aboutToBeDestroyed()

    This signal is emitted before the underlying native OpenGL context is
    destroyed, such that users may clean up OpenGL resources that might
    otherwise be left dangling in the case of shared OpenGL contexts.

    If you wish to make the context current in order to do clean-up, make sure
    to only connect to the signal using a direct connection.

    \note In Qt for Python, this signal will not be received when emitted
    from the destructor of QOpenGLWidget or QOpenGLWindow due to the Python
    instance already being destroyed. We recommend doing cleanups
    in QWidget::hideEvent() instead.
*/

/*!
    Destroys the QOpenGLContext object.

    If this is the current context for the thread, doneCurrent() is also called.
*/
QOpenGLContext::~QOpenGLContext()
{
    destroy();

#ifndef QT_NO_DEBUG
    QOpenGLContextPrivate::cleanMakeCurrentTracker(this);
#endif
}

/*!
    Returns if this context is valid, i.e. has been successfully created.

    On some platforms the return value of \c false for a context that was
    successfully created previously indicates that the OpenGL context was lost.

    The typical way to handle context loss scenarios in applications is to
    check via this function whenever makeCurrent() fails and returns \c false.
    If this function then returns \c false, recreate the underlying native
    OpenGL context by calling create(), call makeCurrent() again and then
    reinitialize all OpenGL resources.

    On some platforms context loss situations is not something that can
    avoided. On others however, they may need to be opted-in to. This can be
    done by enabling \l{QSurfaceFormat::ResetNotification}{ResetNotification} in
    the QSurfaceFormat. This will lead to setting
    \c{RESET_NOTIFICATION_STRATEGY_EXT} to \c{LOSE_CONTEXT_ON_RESET_EXT} in the
    underlying native OpenGL context. QOpenGLContext will then monitor the
    status via \c{glGetGraphicsResetStatusEXT()} in every makeCurrent().

    \sa create()
*/
bool QOpenGLContext::isValid() const
{
    Q_D(const QOpenGLContext);
    return d->platformGLContext && d->platformGLContext->isValid();
}

/*!
    Get the QOpenGLFunctions instance for this context.

    QOpenGLContext offers this as a convenient way to access QOpenGLFunctions
    without having to manage it manually.

    The context or a sharing context must be current.

    The returned QOpenGLFunctions instance is ready to be used and it
    does not need initializeOpenGLFunctions() to be called.
*/
QOpenGLFunctions *QOpenGLContext::functions() const
{
    Q_D(const QOpenGLContext);
    if (!d->functions)
        const_cast<QOpenGLFunctions *&>(d->functions) = new QOpenGLExtensions(QOpenGLContext::currentContext());
    return d->functions;
}

/*!
    Get the QOpenGLExtraFunctions instance for this context.

    QOpenGLContext offers this as a convenient way to access QOpenGLExtraFunctions
    without having to manage it manually.

    The context or a sharing context must be current.

    The returned QOpenGLExtraFunctions instance is ready to be used and it
    does not need initializeOpenGLFunctions() to be called.

    \note QOpenGLExtraFunctions contains functionality that is not guaranteed to
    be available at runtime. Runtime availability depends on the platform,
    graphics driver, and the OpenGL version requested by the application.

    \sa QOpenGLFunctions, QOpenGLExtraFunctions
*/
QOpenGLExtraFunctions *QOpenGLContext::extraFunctions() const
{
    return static_cast<QOpenGLExtraFunctions *>(functions());
}

/*!
    Returns the set of OpenGL extensions supported by this context.

    The context or a sharing context must be current.

    \sa hasExtension()
*/
QSet<QByteArray> QOpenGLContext::extensions() const
{
    Q_D(const QOpenGLContext);
    if (d->extensionNames.isEmpty()) {
        QOpenGLExtensionMatcher matcher;
        d->extensionNames = matcher.extensions();
    }

    return d->extensionNames;
}

/*!
    Returns \c true if this OpenGL context supports the specified OpenGL
    \a extension, \c false otherwise.

    The context or a sharing context must be current.

    \sa extensions()
*/
bool QOpenGLContext::hasExtension(const QByteArray &extension) const
{
    return extensions().contains(extension);
}

/*!
    Call this to get the default framebuffer object for the current surface.

    On some platforms (for instance, iOS) the default framebuffer object depends
    on the surface being rendered to, and might be different from 0. Thus,
    instead of calling glBindFramebuffer(0), you should call
    glBindFramebuffer(ctx->defaultFramebufferObject()) if you want your
    application to work across different Qt platforms.

    If you use the glBindFramebuffer() in QOpenGLFunctions you do not have to
    worry about this, as it automatically binds the current context's
    defaultFramebufferObject() when 0 is passed.

    \note Widgets that render via framebuffer objects, like QOpenGLWidget and
    QQuickWidget, will override the value returned from this function when
    painting is active, because at that time the correct "default" framebuffer
    is the widget's associated backing framebuffer, not the platform-specific
    one belonging to the top-level window's surface. This ensures the expected
    behavior for this function and other classes relying on it (for example,
    QOpenGLFramebufferObject::bindDefault() or
    QOpenGLFramebufferObject::release()).

    \sa QOpenGLFramebufferObject
*/
GLuint QOpenGLContext::defaultFramebufferObject() const
{
    if (!isValid())
        return 0;

    Q_D(const QOpenGLContext);
    if (!d->surface || !d->surface->surfaceHandle())
        return 0;

    if (d->defaultFboRedirect)
        return d->defaultFboRedirect;

    return d->platformGLContext->defaultFramebufferObject(d->surface->surfaceHandle());
}

/*!
    Makes the context current in the current thread, against the given
    \a surface. Returns \c true if successful; otherwise returns \c false.
    The latter may happen if the surface is not exposed, or the graphics
    hardware is not available due to e.g. the application being suspended.

    If \a surface is \nullptr this is equivalent to calling doneCurrent().

    Avoid calling this function from a different thread than the one the
    QOpenGLContext instance lives in. If you wish to use QOpenGLContext from a
    different thread you should first make sure it's not current in the
    current thread, by calling doneCurrent() if necessary. Then call
    moveToThread(otherThread) before using it in the other thread.

    By default Qt employs a check that enforces the above condition on the
    thread affinity. It is still possible to disable this check by setting the
    \c{Qt::AA_DontCheckOpenGLContextThreadAffinity} application attribute. Be
    sure to understand the consequences of using QObjects from outside
    the thread they live in, as explained in the
    \l{QObject#Thread Affinity}{QObject thread affinity} documentation.

    \sa functions(), doneCurrent(), Qt::AA_DontCheckOpenGLContextThreadAffinity
*/
bool QOpenGLContext::makeCurrent(QSurface *surface)
{
    Q_D(QOpenGLContext);
    if (!isValid())
        return false;

    if (Q_UNLIKELY(!qApp->testAttribute(Qt::AA_DontCheckOpenGLContextThreadAffinity)
                   && thread() != QThread::currentThread())) {
        qFatal("Cannot make QOpenGLContext current in a different thread");
    }

    if (!surface) {
        doneCurrent();
        return true;
    }

    if (!surface->surfaceHandle())
        return false;
    if (!surface->supportsOpenGL()) {
        qWarning() << "QOpenGLContext::makeCurrent() called with non-opengl surface" << surface;
        return false;
    }

    if (!d->platformGLContext->makeCurrent(surface->surfaceHandle()))
        return false;

    QOpenGLContextPrivate::setCurrentContext(this);
#ifndef QT_NO_DEBUG
    QOpenGLContextPrivate::toggleMakeCurrentTracker(this, true);
#endif

    d->surface = surface;

    static bool needsWorkaroundSet = false;
    static bool needsWorkaround = false;

    if (!needsWorkaroundSet) {
        QByteArray env;
#ifdef Q_OS_ANDROID
        env = qgetenv(QByteArrayLiteral("QT_ANDROID_DISABLE_GLYPH_CACHE_WORKAROUND"));
        needsWorkaround = env.isEmpty() || env == QByteArrayLiteral("0") || env == QByteArrayLiteral("false");
#endif
        env = qgetenv(QByteArrayLiteral("QT_ENABLE_GLYPH_CACHE_WORKAROUND"));
        if (env == QByteArrayLiteral("1") || env == QByteArrayLiteral("true"))
            needsWorkaround = true;

        if (!needsWorkaround) {
            const char *rendererString = reinterpret_cast<const char *>(functions()->glGetString(GL_RENDERER));
            if (rendererString)
                needsWorkaround =
                        qstrncmp(rendererString, "Mali-4xx", 6) == 0 // Mali-400, Mali-450
                        || qstrcmp(rendererString, "Mali-T880") == 0
                        || qstrncmp(rendererString, "Adreno (TM) 2xx", 13) == 0 // Adreno 200, 203, 205
                        || qstrncmp(rendererString, "Adreno 2xx", 8) == 0 // Same as above but without the '(TM)'
                        || qstrncmp(rendererString, "Adreno (TM) 3xx", 13) == 0 // Adreno 302, 305, 320, 330
                        || qstrncmp(rendererString, "Adreno 3xx", 8) == 0 // Same as above but without the '(TM)'
                        || qstrncmp(rendererString, "Adreno (TM) 4xx", 13) == 0 // Adreno 405, 418, 420, 430
                        || qstrncmp(rendererString, "Adreno 4xx", 8) == 0 // Same as above but without the '(TM)'
                        || qstrncmp(rendererString, "Adreno (TM) 5xx", 13) == 0 // Adreno 505, 506, 510, 530, 540
                        || qstrncmp(rendererString, "Adreno 5xx", 8) == 0 // Same as above but without the '(TM)'
                        || qstrncmp(rendererString, "Adreno (TM) 6xx", 13) == 0 // Adreno 610, 620, 630
                        || qstrncmp(rendererString, "Adreno 6xx", 8) == 0 // Same as above but without the '(TM)'
                        || qstrcmp(rendererString, "GC800 core") == 0
                        || qstrcmp(rendererString, "GC1000 core") == 0
                        || strstr(rendererString, "GC2000") != nullptr
                        || qstrcmp(rendererString, "Immersion.16") == 0
                        || qstrncmp(rendererString, "Apple Mx", 7) == 0;
        }
        needsWorkaroundSet = true;
    }

    if (needsWorkaround)
        d->workaround_brokenFBOReadBack = true;

    d->shareGroup->d_func()->deletePendingResources(this);

    return true;
}

/*!
    Convenience function for calling makeCurrent with a 0 surface.

    This results in no context being current in the current thread.

    \sa makeCurrent(), currentContext()
*/
void QOpenGLContext::doneCurrent()
{
    Q_D(QOpenGLContext);
    if (!isValid())
        return;

    if (QOpenGLContext::currentContext() == this)
        d->shareGroup->d_func()->deletePendingResources(this);

    d->platformGLContext->doneCurrent();
    QOpenGLContextPrivate::setCurrentContext(nullptr);

    d->surface = nullptr;
}

/*!
    Returns the surface the context has been made current with.

    This is the surface passed as an argument to makeCurrent().
*/
QSurface *QOpenGLContext::surface() const
{
    Q_D(const QOpenGLContext);
    return d->surface;
}


/*!
    Swap the back and front buffers of \a surface.

    Call this to finish a frame of OpenGL rendering, and make sure to
    call makeCurrent() again before issuing any further OpenGL commands,
    for example as part of a new frame.
*/
void QOpenGLContext::swapBuffers(QSurface *surface)
{
    Q_D(QOpenGLContext);
    if (!isValid())
        return;

    if (!surface) {
        qWarning("QOpenGLContext::swapBuffers() called with null argument");
        return;
    }

    if (!surface->supportsOpenGL()) {
        qWarning("QOpenGLContext::swapBuffers() called with non-opengl surface");
        return;
    }

    QPlatformSurface *surfaceHandle = surface->surfaceHandle();
    if (!surfaceHandle)
        return;

#if !defined(QT_NO_DEBUG)
    if (!QOpenGLContextPrivate::toggleMakeCurrentTracker(this, false))
        qWarning("QOpenGLContext::swapBuffers() called without corresponding makeCurrent()");
#endif
    if (surface->format().swapBehavior() == QSurfaceFormat::SingleBuffer)
        functions()->glFlush();
    d->platformGLContext->swapBuffers(surfaceHandle);
}

/*!
    Resolves the function pointer to an OpenGL extension function, identified by \a procName

    Returns \nullptr if no such function can be found.
*/
QFunctionPointer QOpenGLContext::getProcAddress(const QByteArray &procName) const
{
    return getProcAddress(procName.constData());
}

/*!
  \overload
  \since 5.8
 */
QFunctionPointer QOpenGLContext::getProcAddress(const char *procName) const
{
    Q_D(const QOpenGLContext);
    if (!d->platformGLContext)
        return nullptr;
    return d->platformGLContext->getProcAddress(procName);
}

/*!
    Returns the format of the underlying platform context, if create() has been called.

    Otherwise, returns the requested format.

    The requested and the actual format may differ. Requesting a given OpenGL version does
    not mean the resulting context will target exactly the requested version. It is only
    guaranteed that the version/profile/options combination for the created context is
    compatible with the request, as long as the driver is able to provide such a context.

    For example, requesting an OpenGL version 3.x core profile context may result in an
    OpenGL 4.x core profile context. Similarly, a request for OpenGL 2.1 may result in an
    OpenGL 3.0 context with deprecated functions enabled. Finally, depending on the
    driver, unsupported versions may result in either a context creation failure or in a
    context for the highest supported version.

    Similar differences are possible in the buffer sizes, for example, the resulting
    context may have a larger depth buffer than requested. This is perfectly normal.
*/
QSurfaceFormat QOpenGLContext::format() const
{
    Q_D(const QOpenGLContext);
    if (!d->platformGLContext)
        return d->requestedFormat;
    return d->platformGLContext->format();
}

/*!
    Returns the share group this context belongs to.
*/
QOpenGLContextGroup *QOpenGLContext::shareGroup() const
{
    Q_D(const QOpenGLContext);
    return d->shareGroup;
}

/*!
    Returns the share context this context was created with.

    If the underlying platform was not able to support the requested
    sharing, this will return 0.
*/
QOpenGLContext *QOpenGLContext::shareContext() const
{
    Q_D(const QOpenGLContext);
    return d->shareContext;
}

/*!
    Returns the screen the context was created for.
*/
QScreen *QOpenGLContext::screen() const
{
    Q_D(const QOpenGLContext);
    return d->screen;
}

/*!
  \enum QOpenGLContext::OpenGLModuleType
  This enum defines the type of the underlying OpenGL implementation.

  \value LibGL   OpenGL
  \value LibGLES OpenGL ES 2.0 or higher

  \since 5.3
*/

/*!
  Returns the underlying OpenGL implementation type.

  On platforms where the OpenGL implementation is not dynamically
  loaded, the return value is determined during compile time and never
  changes.

  \note A desktop OpenGL implementation may be capable of creating
  ES-compatible contexts too. Therefore in most cases it is more
  appropriate to check QSurfaceFormat::renderableType() or use
  the convenience function isOpenGLES().

  \note This function requires that the QGuiApplication instance is already created.

  \since 5.3
 */
QOpenGLContext::OpenGLModuleType QOpenGLContext::openGLModuleType()
{
    QByteArray env = qgetenv(QByteArrayLiteral("QT_OPENGL_PREFER_GLES"));
    if (env == QByteArrayLiteral("1") || env == QByteArrayLiteral("true")) {
        qDebug("----------------USE LibGLES");
        return LibGLES;
    }
#if defined(QT_OPENGL_DYNAMIC)
    Q_ASSERT(qGuiApp);
    return QGuiApplicationPrivate::instance()->platformIntegration()->openGLModuleType();
#elif QT_CONFIG(opengles2)
    return LibGLES;
#else
    return LibGL;
#endif
}

/*!
  Returns true if the context is an OpenGL ES context.

  If the context has not yet been created, the result is based on the
  requested format set via setFormat().

  \sa create(), format(), setFormat()

  \since 5.3
  */
bool QOpenGLContext::isOpenGLES() const
{
    return format().renderableType() == QSurfaceFormat::OpenGLES;
}

/*!
  Returns \c true if the platform supports OpenGL rendering outside the main (gui)
  thread.

  The value is controlled by the platform plugin in use and may also depend on the
  graphics drivers.

  \since 5.5
 */
bool QOpenGLContext::supportsThreadedOpenGL()
{
    Q_ASSERT(qGuiApp);
    return QGuiApplicationPrivate::instance()->platformIntegration()->hasCapability(QPlatformIntegration::ThreadedOpenGL);
}

/*!
    \since 5.5

    Returns the application-wide shared OpenGL context, if present.
    Otherwise, returns \nullptr.

    This is useful if you need to upload OpenGL objects (buffers, textures,
    etc.) before creating or showing a QOpenGLWidget or QQuickWidget.

    \note You must set the Qt::AA_ShareOpenGLContexts flag on QGuiApplication
    before creating the QGuiApplication object, otherwise Qt may not create a
    global shared context.

    \warning Do not attempt to make the context returned by this function
    current on any surface. Instead, you can create a new context which shares
    with the global one, and then make the new context current.

    \sa Qt::AA_ShareOpenGLContexts, setShareContext(), makeCurrent()
*/
QOpenGLContext *QOpenGLContext::globalShareContext()
{
    Q_ASSERT(qGuiApp);
    return qt_gl_global_share_context();
}

/*!
    \internal
*/
QOpenGLTextureHelper* QOpenGLContext::textureFunctions() const
{
    Q_D(const QOpenGLContext);
    return d->textureFunctions;
}

/*!
    \internal
*/
void QOpenGLContext::setTextureFunctions(QOpenGLTextureHelper* textureFuncs, std::function<void()> destroyCallback)
{
    Q_D(QOpenGLContext);
    d->textureFunctions = textureFuncs;
    d->textureFunctionsDestroyCallback = destroyCallback;
}

/*!
    \class QOpenGLContextGroup
    \since 5.0
    \brief The QOpenGLContextGroup class represents a group of contexts sharing
    OpenGL resources.
    \inmodule QtGui

    QOpenGLContextGroup is automatically created and managed by QOpenGLContext
    instances.  Its purpose is to identify all the contexts that are sharing
    resources.

    \sa QOpenGLContext::shareGroup()
*/
QOpenGLContextGroup::QOpenGLContextGroup()
    : QObject(*new QOpenGLContextGroupPrivate())
{
}

/*!
    \internal
*/
QOpenGLContextGroup::~QOpenGLContextGroup()
{
    Q_D(QOpenGLContextGroup);
    d->cleanup();
}

/*!
    Returns all the QOpenGLContext objects in this share group.
*/
QList<QOpenGLContext *> QOpenGLContextGroup::shares() const
{
    Q_D(const QOpenGLContextGroup);
    return d->m_shares;
}

/*!
    Returns the QOpenGLContextGroup corresponding to the current context.

    \sa QOpenGLContext::currentContext()
*/
QOpenGLContextGroup *QOpenGLContextGroup::currentContextGroup()
{
    QOpenGLContext *current = QOpenGLContext::currentContext();
    return current ? current->shareGroup() : nullptr;
}

QOpenGLContextGroupPrivate::~QOpenGLContextGroupPrivate()
    = default;

void QOpenGLContextGroupPrivate::addContext(QOpenGLContext *ctx)
{
    const auto locker = qt_scoped_lock(m_mutex);
    m_refs.ref();
    m_shares << ctx;
}

void QOpenGLContextGroupPrivate::removeContext(QOpenGLContext *ctx)
{
    Q_Q(QOpenGLContextGroup);

    bool deleteObject = false;

    {
        const auto locker = qt_scoped_lock(m_mutex);
        m_shares.removeOne(ctx);

        if (ctx == m_context && !m_shares.isEmpty())
            m_context = m_shares.constFirst();

        if (!m_refs.deref()) {
            cleanup();
            deleteObject = true;
        }
    }

    if (deleteObject) {
        if (q->thread() == QThread::currentThread())
            delete q; // Delete directly to prevent leak, refer to QTBUG-29056
        else
            q->deleteLater();
    }
}

void QOpenGLContextGroupPrivate::cleanup()
{
    Q_Q(QOpenGLContextGroup);
    {
        QHash<QOpenGLMultiGroupSharedResource *, QOpenGLSharedResource *>::const_iterator it, end;
        end = m_resources.constEnd();
        for (it = m_resources.constBegin(); it != end; ++it)
            it.key()->cleanup(q, it.value());
        m_resources.clear();
    }

    QList<QOpenGLSharedResource *>::iterator it = m_sharedResources.begin();
    QList<QOpenGLSharedResource *>::iterator end = m_sharedResources.end();

    while (it != end) {
        (*it)->invalidateResource();
        (*it)->m_group = nullptr;
        ++it;
    }

    m_sharedResources.clear();

    qDeleteAll(m_pendingDeletion.begin(), m_pendingDeletion.end());
    m_pendingDeletion.clear();
}

void QOpenGLContextGroupPrivate::deletePendingResources(QOpenGLContext *ctx)
{
    const auto locker = qt_scoped_lock(m_mutex);

    const QList<QOpenGLSharedResource *> pending = m_pendingDeletion;
    m_pendingDeletion.clear();

    QList<QOpenGLSharedResource *>::const_iterator it = pending.begin();
    QList<QOpenGLSharedResource *>::const_iterator end = pending.end();
    while (it != end) {
        (*it)->freeResource(ctx);
        delete *it;
        ++it;
    }
}

/*!
    \class QOpenGLSharedResource
    \internal
    \since 5.0
    \brief The QOpenGLSharedResource class is used to keep track of resources
    that are shared between OpenGL contexts (like textures, framebuffer
    objects, shader programs, etc), and clean them up in a safe way when
    they're no longer needed.
    \inmodule QtGui

    The QOpenGLSharedResource instance should never be deleted, instead free()
    should be called when it's no longer needed. Thus it will be put on a queue
    and freed at an appropriate time (when a context in the share group becomes
    current).

    The sub-class needs to implement two pure virtual functions. The first,
    freeResource() must be implemented to actually do the freeing, for example
    call glDeleteTextures() on a texture id. Qt makes sure a valid context in
    the resource's share group is current at the time. The other,
    invalidateResource(), is called by Qt in the circumstance when the last
    context in the share group is destroyed before free() has been called. The
    implementation of invalidateResource() should set any identifiers to 0 or
    set a flag to prevent them from being used later on.
*/
QOpenGLSharedResource::QOpenGLSharedResource(QOpenGLContextGroup *group)
    : m_group(group)
{
    const auto locker = qt_scoped_lock(m_group->d_func()->m_mutex);
    m_group->d_func()->m_sharedResources << this;
}

QOpenGLSharedResource::~QOpenGLSharedResource()
{
}

// schedule the resource for deletion at an appropriate time
void QOpenGLSharedResource::free()
{
    if (!m_group) {
        delete this;
        return;
    }

    const auto locker = qt_scoped_lock(m_group->d_func()->m_mutex);
    m_group->d_func()->m_sharedResources.removeOne(this);
    m_group->d_func()->m_pendingDeletion << this;

    // can we delete right away?
    QOpenGLContext *current = QOpenGLContext::currentContext();
    if (current && current->shareGroup() == m_group) {
        m_group->d_func()->deletePendingResources(current);
    }
}

/*!
    \class QOpenGLSharedResourceGuard
    \internal
    \since 5.0
    \brief The QOpenGLSharedResourceGuard class is a convenience sub-class of
    QOpenGLSharedResource to be used to track a single OpenGL object with a
    GLuint identifier. The constructor takes a function pointer to a function
    that will be used to free the resource if and when necessary.
    \inmodule QtGui

*/

QOpenGLSharedResourceGuard::~QOpenGLSharedResourceGuard()
    = default;

void QOpenGLSharedResourceGuard::freeResource(QOpenGLContext *context)
{
    if (m_id) {
        QOpenGLFunctions functions(context);
        m_func(&functions, m_id);
        m_id = 0;
    }
}

/*!
    \class QOpenGLMultiGroupSharedResource
    \internal
    \since 5.0
    \brief The QOpenGLMultiGroupSharedResource keeps track of a shared resource
    that might be needed from multiple contexts, like a glyph cache or gradient
    cache. One instance of the object is created for each group when necessary.
    The shared resource instance should have a constructor that takes a
    QOpenGLContext *. To get an instance for a given context one calls
    T *QOpenGLMultiGroupSharedResource::value<T>(context), where T is a sub-class
    of QOpenGLSharedResource.
    \inmodule QtGui

    You should not call free() on QOpenGLSharedResources owned by a
    QOpenGLMultiGroupSharedResource instance.
*/
QOpenGLMultiGroupSharedResource::QOpenGLMultiGroupSharedResource()
    : active(0)
{
#ifdef QT_GL_CONTEXT_RESOURCE_DEBUG
    qDebug("Creating context group resource object %p.", this);
#endif
}

QOpenGLMultiGroupSharedResource::~QOpenGLMultiGroupSharedResource()
{
#ifdef QT_GL_CONTEXT_RESOURCE_DEBUG
    qDebug("Deleting context group resource %p. Group size: %d.", this, m_groups.size());
#endif
    for (int i = 0; i < m_groups.size(); ++i) {
        if (!m_groups.at(i)->shares().isEmpty()) {
            QOpenGLContext *context = m_groups.at(i)->shares().constFirst();
            QOpenGLSharedResource *resource = value(context);
            if (resource)
                resource->free();
        }
        m_groups.at(i)->d_func()->m_resources.remove(this);
        active.deref();
    }
#ifndef QT_NO_DEBUG
    if (active.loadRelaxed() != 0) {
        qWarning("QtGui: Resources are still available at program shutdown.\n"
                 "          This is possibly caused by a leaked QOpenGLWidget, \n"
                 "          QOpenGLFramebufferObject or QOpenGLPixelBuffer.");
    }
#endif
}

void QOpenGLMultiGroupSharedResource::insert(QOpenGLContext *context, QOpenGLSharedResource *value)
{
#ifdef QT_GL_CONTEXT_RESOURCE_DEBUG
    qDebug("Inserting context group resource %p for context %p, managed by %p.", value, context, this);
#endif
    QOpenGLContextGroup *group = context->shareGroup();
    Q_ASSERT(!group->d_func()->m_resources.contains(this));
    group->d_func()->m_resources.insert(this, value);
    m_groups.append(group);
    active.ref();
}

QOpenGLSharedResource *QOpenGLMultiGroupSharedResource::value(QOpenGLContext *context)
{
    QOpenGLContextGroup *group = context->shareGroup();
    return group->d_func()->m_resources.value(this, nullptr);
}

QList<QOpenGLSharedResource *> QOpenGLMultiGroupSharedResource::resources() const
{
    QList<QOpenGLSharedResource *> result;
    for (QList<QOpenGLContextGroup *>::const_iterator it = m_groups.constBegin(); it != m_groups.constEnd(); ++it) {
        QOpenGLSharedResource *resource = (*it)->d_func()->m_resources.value(const_cast<QOpenGLMultiGroupSharedResource *>(this), nullptr);
        if (resource)
            result << resource;
    }
    return result;
}

void QOpenGLMultiGroupSharedResource::cleanup(QOpenGLContextGroup *group, QOpenGLSharedResource *value)
{
#ifdef QT_GL_CONTEXT_RESOURCE_DEBUG
    qDebug("Cleaning up context group resource %p, for group %p in thread %p.", this, group, QThread::currentThread());
#endif
    value->invalidateResource();
    value->free();
    active.deref();

    Q_ASSERT(m_groups.contains(group));
    m_groups.removeOne(group);
}

QOpenGLContextVersionFunctionHelper::~QOpenGLContextVersionFunctionHelper()
    = default;

#ifndef QT_NO_DEBUG_STREAM
QDebug operator<<(QDebug debug, const QOpenGLContext *ctx)
{
    QDebugStateSaver saver(debug);
    debug.nospace();
    debug.noquote();
    debug << "QOpenGLContext(";
    if (ctx)  {
        debug << static_cast<const void *>(ctx);
        if (ctx->isValid()) {
            debug << ", format=" << ctx->format();
            if (const QSurface *sf = ctx->surface())
                debug << ", surface=" << sf;
            if (const QScreen *s = ctx->screen())
                debug << ", screen=\"" << s->name() << '"';
        } else {
            debug << ", invalid";
        }
    } else {
        debug << '0';
    }
    debug << ')';
    return debug;
}

QDebug operator<<(QDebug debug, const QOpenGLContextGroup *cg)
{
    QDebugStateSaver saver(debug);
    debug.nospace();
    debug << "QOpenGLContextGroup(";
    if (cg)
        debug << cg->shares();
    else
        debug << '0';
    debug << ')';
    return debug;
}
#endif // QT_NO_DEBUG_STREAM

using namespace QNativeInterface;

void *QOpenGLContext::resolveInterface(const char *name, int revision) const
{
    Q_UNUSED(name); Q_UNUSED(revision);

    auto *platformContext = handle();
    Q_UNUSED(platformContext);

#if defined(Q_OS_MACOS)
    QT_NATIVE_INTERFACE_RETURN_IF(QCocoaGLContext, platformContext);
#endif
#if defined(Q_OS_WIN)
    QT_NATIVE_INTERFACE_RETURN_IF(QWGLContext, platformContext);
#endif
#if QT_CONFIG(xcb_glx_plugin)
    QT_NATIVE_INTERFACE_RETURN_IF(QGLXContext, platformContext);
#endif
#if QT_CONFIG(egl)
    QT_NATIVE_INTERFACE_RETURN_IF(QEGLContext, platformContext);
#endif

    return nullptr;
}

QT_END_NAMESPACE

#include "moc_qopenglcontext.cpp"
