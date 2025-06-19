#include "mpv.h"

#include <stdexcept>
#include <clocale>

#include <QObject>
#include <QJsonObject>
#include <QCoreApplication>

#include <QOpenGLContext>
#include <QOpenGLFramebufferObject>
#include <QQuickWindow>
#include <QQuickView>

#if defined(Q_OS_WIN32)
#include <windows.h>
#include <dwmapi.h>
#pragma comment(lib, "dwmapi.lib")
#endif

namespace {
void on_mpv_redraw(void *ctx)
{
    MpvObject::on_update(ctx);
}

void *get_proc_address_mpv(void *ctx, const char *name)
{
    Q_UNUSED(ctx)
    QOpenGLContext *glctx = QOpenGLContext::currentContext();
    if (!glctx) return nullptr;
    return reinterpret_cast<void *>(glctx->getProcAddress(QByteArray(name)));
}
} // namespace

class MpvRenderer : public QQuickFramebufferObject::Renderer
{
    MpvObject *obj;

public:
    explicit MpvRenderer(MpvObject *new_obj)
        : obj{new_obj}
    {
        std::setlocale(LC_NUMERIC, "C");
    }

    ~MpvRenderer() override = default;

    QOpenGLFramebufferObject *createFramebufferObject(const QSize &size) override
    {
        if (!obj->mpv_gl)
        {
            mpv_opengl_init_params gl_init_params{get_proc_address_mpv, nullptr, nullptr};
            mpv_render_param params[] = {
                {MPV_RENDER_PARAM_API_TYPE, const_cast<char *>(MPV_RENDER_API_TYPE_OPENGL)},
                {MPV_RENDER_PARAM_OPENGL_INIT_PARAMS, &gl_init_params},
                {MPV_RENDER_PARAM_INVALID, nullptr}};

            if (mpv_render_context_create(&obj->mpv_gl, obj->mpv, params) < 0)
                throw std::runtime_error("failed to initialize mpv GL context");

            mpv_render_context_set_update_callback(obj->mpv_gl, on_mpv_redraw, obj);
        }

        return QQuickFramebufferObject::Renderer::createFramebufferObject(size);
    }

    void render() override
    {

        QOpenGLFramebufferObject *fbo = framebufferObject();
        mpv_opengl_fbo mpfbo{static_cast<int>(fbo->handle()), fbo->width(), fbo->height(), 0};
        int flip_y = 0;

        mpv_render_param params[] = {
            {MPV_RENDER_PARAM_OPENGL_FBO, &mpfbo},
            {MPV_RENDER_PARAM_FLIP_Y, &flip_y},
            {MPV_RENDER_PARAM_INVALID, nullptr}};

        mpv_render_context_render(obj->mpv_gl, params);

    }
};

MpvObject::MpvObject(QQuickItem *parent)
    : QQuickFramebufferObject(parent), mpv{mpv_create()}, mpv_gl(nullptr)
{
#ifdef Q_OS_WIN32
    DwmEnableMMCSS(TRUE);
#endif

    if (!mpv)
        throw std::runtime_error("could not create mpv context");

    connect(this, &MpvObject::onUpdate, this, &MpvObject::doUpdate, Qt::QueuedConnection);

    initialize_mpv();

    setVisible(false);
    observeProperty("vid");
}

MpvObject::~MpvObject()
{
    if (mpv_gl)
        mpv_render_context_free(mpv_gl);
    mpv_terminate_destroy(mpv);
}

void MpvObject::initialize_mpv()
{
    mpv_set_option_string(mpv, "terminal", "yes");
    mpv_set_option_string(mpv, "msg-level", "all=v");

    if (mpv_initialize(mpv) < 0)
        throw std::runtime_error("could not initialize mpv context");

    mpv::qt::set_property(mpv, "vo", "libmpv");
    mpv::qt::set_property(mpv, "gpu-hwdec-interop", "auto");
    mpv::qt::set_property(mpv, "cache-default", 15000);
    mpv::qt::set_property(mpv, "cache-backbuffer", 15000);
    mpv::qt::set_property(mpv, "cache-secs", 10);
    mpv::qt::set_property(mpv, "audio-client-name", QCoreApplication::applicationName());
    mpv::qt::set_property(mpv, "title", QCoreApplication::applicationName());
    mpv::qt::set_property(mpv, "audio-fallback-to-null", "yes");

    mpv_set_wakeup_callback(mpv, wakeup, this);

    for (const QString &name : std::as_const(observed_properties)) {
        mpv_observe_property(mpv, 0, name.toStdString().c_str(), MPV_FORMAT_NODE);
    }
}

void MpvObject::on_update(void *ctx)
{
    emit static_cast<MpvObject *>(ctx)->onUpdate();
}

void MpvObject::doUpdate()
{
    update();
}

void MpvObject::command(const QVariant &params)
{
    mpv::qt::command(mpv, params);
}

void MpvObject::setProperty(const QString &name, const QVariant &value)
{
    mpv::qt::set_property(mpv, name, value);
}

void MpvObject::observeProperty(const QString &name)
{
    observed_properties.insert(name);
    mpv_observe_property(mpv, 0, name.toStdString().c_str(), MPV_FORMAT_NODE);
}

QVariant MpvObject::getProperty(const QString &name)
{
    return mpv::qt::get_property(mpv, name);
}

void MpvObject::wakeup(void *ctx)
{
    QMetaObject::invokeMethod(static_cast<MpvObject *>(ctx), "on_mpv_events", Qt::QueuedConnection);
}

void MpvObject::on_mpv_events()
{
    while (mpv)
    {
        mpv_event *event = mpv_wait_event(mpv, 0);
        if (event->event_id == MPV_EVENT_NONE)
            break;
        handle_mpv_event(event);
    }
}

void MpvObject::handle_mpv_event(mpv_event *event)
{
    QJsonObject eventJson;
    eventJson["id"] = qint64(event->reply_userdata);

    if (event->error < 0)
        eventJson["error"] = QString::fromUtf8(mpv_error_string(event->error));

    switch (event->event_id)
    {
    case MPV_EVENT_PROPERTY_CHANGE:
    {
        auto *prop = static_cast<mpv_event_property *>(event->data);
        eventJson["name"] = QString::fromUtf8(prop->name);

        switch (prop->format)
        {
        case MPV_FORMAT_NODE:
            if (((mpv_node *)prop->data)->format == MPV_FORMAT_INT64 && eventJson["name"] == "vid")
                setVisible(true);
            eventJson["data"] = QJsonValue::fromVariant(mpv::qt::node_to_variant((mpv_node *)prop->data));
            break;
        case MPV_FORMAT_DOUBLE:
            eventJson["data"] = *(double *)prop->data;
            break;
        case MPV_FORMAT_FLAG:
            eventJson["data"] = *(int *)prop->data;
            break;
        case MPV_FORMAT_STRING:
            eventJson["data"] = QString::fromUtf8(*(char **)prop->data);
            break;
        default:
            break;
        }

        emit mpvEvent("mpv-prop-change", eventJson);
        break;
    }
    case MPV_EVENT_END_FILE:
    {
        setVisible(false);
        auto *endFile = static_cast<mpv_event_end_file *>(event->data);
        switch (endFile->reason)
        {
        case MPV_END_FILE_REASON_ERROR:
            eventJson["reason"] = "error";
            eventJson["error"] = mpv_error_string(endFile->error);
            break;
        case MPV_END_FILE_REASON_QUIT:
            eventJson["reason"] = "quit";
            break;
        default:
            eventJson["reason"] = "other";
            break;
        }
        emit mpvEvent("mpv-event-ended", eventJson);
        break;
    }
    case MPV_EVENT_SHUTDOWN:
    {
        if (mpv_gl)
        {
            mpv_render_context_free(mpv_gl);
            mpv_gl = nullptr;
        }
        mpv_terminate_destroy(mpv);
        mpv = mpv_create();
        initialize_mpv();
        break;
    }
    default:
        break;
    }
}

QQuickFramebufferObject::Renderer *MpvObject::createRenderer() const
{
    return new MpvRenderer(const_cast<MpvObject *>(this));
}
