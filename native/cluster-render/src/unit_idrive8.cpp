/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 IterioDev */
/* cluster-render-idrive8: the service on iDrive 8 (README.md "iDrive 8"):
 * a RAMSES 27 participant (communication user ramses_user, 801 VERIFIED on
 * the car) whose built-in DCSM provider offers each target's scene to the
 * cluster (dcsm27_port.h). The provider has no file descriptor: its events
 * are dispatched every pass, and the loop wakes at least every
 * kDispatchMs. A target's scene goes once its offer has stopped
 * (graph_ramses27.h). Any crash on the unit restarts the head unit
 * (VERIFIED on the car), the teardown of RAMSES 27 included: once the
 * offers are stopped and the framework is disconnected, the process leaves
 * with _exit(), running no destructor. RAMSES not up yet (at boot) is
 * waited for here, not left to systemd's restart: what iDrive 8 does when
 * a service exits is CAR-ONLY (a stopped service restarted the head unit
 * on the car). */
#include <cerrno>
#include <csignal>
#include <cstring>
#include <pthread.h>
#include <sys/signalfd.h>
#include <unistd.h>

#include <algorithm>
#include <codecvt>
#include <locale>

#include "dcsm27_port.h"
#include "graph_ramses27.h"
#include "log.h"
#include "ramses-client-api/RamsesClient.h"
#include "ramses-framework-api/CategoryInfoUpdate.h"
#include "ramses-framework-api/DcsmMetadataCreator.h"
#include "ramses-framework-api/DcsmProvider.h"
#include "ramses-framework-api/DcsmStatusMessage.h"
#include "ramses-framework-api/IDcsmProviderEventHandler.h"
#include "ramses-framework-api/RamsesFramework.h"
#include "ramses-framework-api/RamsesFrameworkConfig.h"
#include "settings.h"
#include "unit.h"

/* In the unit's libramses 27.0.116r1, not in the open-source headers
 * (../abi/ABI27.md): SOME/IP as the given communication user. */
ramses::status_t enable_someip(ramses::RamsesFrameworkConfig *cfg, uint32_t user)
    __asm__("_ZN6ramses21RamsesFrameworkConfig25enableSomeIPCommunicationEj");

namespace crs {

namespace {

const int kDispatchMs = 20;
/* How long a stopping offer may take to be accepted at shutdown. */
const int kStopMs = 1000;
/* How often a failed connect is tried again. */
const int kConnectRetryMs = 5000;

ramses::ContentID content_id(uint64_t c) { return ramses::ContentID(c); }

/* The offers, with a widget's menu entry (its description, order and
 * picture) for a target that has an id8_<name>_description, its
 * background (id8_<name>_background_id) and the cluster views it is shown
 * in (id8_<name>_layouts). A target with id8_<name>_focus 1 asks the
 * cluster to show it with every offer: the cluster switches to a widget
 * that asks by itself, nobody picks it (VERIFIED on the car, below). */
class ProviderWire : public Dcsm27Wire {
public:
    ProviderWire(ramses::DcsmProvider &p, const cld::Config &cfg) : p_(p), cfg_(cfg) {}

    bool offer(const TargetSpec &t, std::string *why) override
    {
        const std::string k = "id8_" + t.name + "_";
        const std::string desc = cfg_.str((k + "description").c_str());
        const bool background = cfg_.has((k + "background_id").c_str()), layouts = cfg_.has((k + "layouts").c_str());
        ramses::status_t s;
        if (desc.empty() && !background && !layouts) {
            s = p_.offerContent(content_id(t.content), ramses::Category(t.slot), ramses::sceneId_t(t.scene_id),
                                ramses::EDcsmOfferingMode::LocalAndRemote);
        } else {
            ramses::DcsmMetadataCreator md;
            if (!desc.empty())
                menu_entry(t, k, desc, &md);
            if (background)
                md.setWidgetBackgroundID((int32_t)cfg_.num((k + "background_id").c_str()));
            if (layouts)
                md.setLayoutAvailability((uint8_t)cfg_.num((k + "layouts").c_str()));
            s = p_.offerContentWithMetadata(content_id(t.content), ramses::Category(t.slot),
                                            ramses::sceneId_t(t.scene_id), ramses::EDcsmOfferingMode::LocalAndRemote,
                                            md);
        }
        if (s != ramses::StatusOK) {
            *why = p_.getStatusMessage(s);
            return false;
        }
        /* The provider keeps the request and sends it once the cluster
         * assigns the content; it goes with the offer. */
        if (cfg_.num((k + "focus").c_str()) != 0) {
            const ramses::status_t f = p_.enableFocusRequest(content_id(t.content), 0);
            if (f != ramses::StatusOK)
                log_warn("target %s: focus request refused: %s", t.name.c_str(), p_.getStatusMessage(f));
        }
        return true;
    }

    bool stop_offer(uint64_t content, std::string *why) override
    {
        const ramses::status_t s = p_.requestStopOfferContent(content_id(content));
        if (s != ramses::StatusOK)
            *why = p_.getStatusMessage(s);
        return s == ramses::StatusOK;
    }

    bool ready(uint64_t content, std::string *why) override
    {
        const ramses::status_t s = p_.markContentReady(content_id(content));
        if (s != ramses::StatusOK)
            *why = p_.getStatusMessage(s);
        return s == ramses::StatusOK;
    }

private:
    void menu_entry(const TargetSpec &t, const std::string &k, const std::string &desc, ramses::DcsmMetadataCreator *md)
    {
        std::u32string text;
        try {
            text = std::wstring_convert<std::codecvt_utf8<char32_t>, char32_t>().from_bytes(desc);
        } catch (const std::range_error &) {
            text = U"?";
        }
        md->setPreviewDescription(text);
        md->setWidgetOrder((int32_t)cfg_.num((k + "order").c_str()));
        const std::string preview = cfg_.str((k + "preview").c_str());
        if (preview.empty())
            return;
        std::vector<char> png;
        std::string why;
        if (read_preview(preview, &png, &why))
            md->setPreviewImagePng(png.data(), png.size());
        else
            log_error("target %s: no picture in %s: %s", t.name.c_str(), preview.c_str(), why.c_str());
    }

    ramses::DcsmProvider &p_;
    const cld::Config &cfg_;
};

/* The provider's events, to the port; the status messages logged. */
class Events : public ramses::IDcsmProviderEventHandlerExtended {
public:
    explicit Events(Dcsm27Port &port) : port_(port) {}
    int64_t now = 0;

    void contentHide(ramses::ContentID c, ramses::AnimationInformation) override { port_.on_hide(c.getValue(), now); }
    void contentShow(ramses::ContentID c, ramses::AnimationInformation) override { port_.on_show(c.getValue(), now); }
    void stopOfferAccepted(ramses::ContentID c, ramses::AnimationInformation) override
    {
        port_.on_stop_accepted(c.getValue(), now);
    }
    void contentSizeChange(ramses::ContentID c, const ramses::CategoryInfoUpdate &ci,
                           ramses::AnimationInformation) override
    {
        ContentArea a;
        if ((a.has_render = ci.hasRenderSizeUpdate())) {
            a.render_w = ci.getRenderSize().width;
            a.render_h = ci.getRenderSize().height;
        }
        if ((a.has_rect = ci.hasCategoryRectUpdate()))
            a.rect = rect(ci.getCategoryRect());
        if ((a.has_safe = ci.hasSafeRectUpdate()))
            a.safe = rect(ci.getSafeRect());
        if ((a.has_layout = ci.hasActiveLayoutUpdate()))
            a.layout = (uint32_t)ci.getActiveLayout();
        port_.on_size(c.getValue(), a, now);
    }
    void contentReadyRequested(ramses::ContentID c) override { port_.on_ready_requested(c.getValue(), now); }
    void contentRelease(ramses::ContentID c, ramses::AnimationInformation) override
    {
        port_.on_release(c.getValue(), now);
    }
    void contentStatus(ramses::ContentID c, const ramses::DcsmStatusMessage &m) override
    {
        const unsigned long long id = c.getValue();
        if (const ramses::StreamStatusMessage *s = m.getAsStreamStatus())
            log_info("dcsm: content %llu: stream status %u", id, (unsigned)s->getStreamStatus());
        else if (const ramses::ActiveLayoutMessage *l = m.getAsActiveLayout())
            log_info("dcsm: content %llu: layout %u", id, (unsigned)l->getLayout());
        else if (const ramses::WidgetFocusStatusMessage *f = m.getAsWidgetFocusStatus())
            log_info("dcsm: content %llu: focus %u", id, (unsigned)f->getWidgetFocusStatus());
    }

private:
    static Rect rect(const ramses::Rect &r)
    {
        Rect o;
        o.x = (int32_t)r.x;
        o.y = (int32_t)r.y;
        o.w = r.width;
        o.h = r.height;
        return o;
    }
    Dcsm27Port &port_;
};

}  // namespace

int start(const cld::Config &cfg, char **)
{
    ServiceConfig sc;
    std::string err;
    if (!service_config(cfg, 8, &sc, &err)) {
        log_error("%s", err.c_str());
        _exit(1);
    }

    sigset_t sigs;
    sigemptyset(&sigs);
    sigaddset(&sigs, SIGINT);
    sigaddset(&sigs, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &sigs, nullptr);
    const int sig_fd = signalfd(-1, &sigs, SFD_CLOEXEC);
    if (sig_fd < 0) {
        log_error("signalfd: %s", strerror(errno));
        _exit(1);
    }

    ramses::RamsesFramework::SetConsoleLogLevel(cfg.num("verbose") ? ramses::ELogLevel::Info : ramses::ELogLevel::Warn);
    ramses::RamsesFrameworkConfig fwc;
    fwc.setDLTApplicationID("CLRD");
    fwc.setDLTApplicationDescription("cluster-render");
    const uint32_t user = (uint32_t)cfg.num("ramses_user");
    if (enable_someip(&fwc, user) != ramses::StatusOK) {
        log_error("ramses: SOME/IP communication user %u refused", user);
        _exit(1);
    }
    ramses::RamsesFramework framework(fwc);
    ramses::RamsesClient *client = framework.createClient("cluster-render");
    ramses::DcsmProvider *provider = framework.createDcsmProvider();
    if (!client || !provider) {
        log_error("ramses: no client or DCSM provider");
        _exit(1);
    }
    /* A failed connect leaves the framework unconnected, to be tried again
     * (upstream's RamsesFrameworkImpl::connect). */
    for (bool said = false; framework.connect() != ramses::StatusOK; said = true) {
        if (!said)
            log_error("ramses: connect failed (communication user %u); trying again every %d s", user,
                      kConnectRetryMs / 1000);
        pollfd p{sig_fd, POLLIN, 0};
        if (poll(&p, 1, kConnectRetryMs) > 0)
            _exit(0);
    }
    log_info("ramses: connected as communication user %u", user);
    Ramses27Gpu gpu(*client);
    if (!gpu.init(&err)) {
        log_error("ramses: %s", err.c_str());
        framework.disconnect();
        _exit(1);
    }

    ProviderWire wire(*provider, cfg);
    Dcsm27Port port(&wire);
    Service service(&gpu, &port, sc);
    port.set_manager(&service.slots());
    port.on_stopped = [&gpu](uint64_t scene) { gpu.drop_retired(scene); };
    gpu.offered = [&port](uint64_t scene) { return port.holds(scene); };
    Events events(port);
    if (!service.start(&err)) {
        log_error("%s", err.c_str());
        framework.disconnect();
        _exit(1);
    }
    port.on_availability(true, cld::now_ms());
    for (const TargetSpec &t : sc.targets)
        log_info("target %s: category %llu content %llu scene 0x%llx %ux%u, linger %d ms", t.name.c_str(),
                 (unsigned long long)t.slot, (unsigned long long)t.content, (unsigned long long)t.scene_id, t.width,
                 t.height, t.linger_ms);

    std::vector<pollfd> fds;
    for (;;) {
        fds.clear();
        fds.push_back(pollfd{sig_fd, POLLIN, 0});
        const size_t first = service.poll_fds(&fds);
        const int timeout = service.timeout_ms(cld::now_ms());
        const int n = poll(fds.data(), fds.size(), timeout < 0 ? kDispatchMs : std::min(timeout, kDispatchMs));
        if (n < 0 && errno != EINTR)
            break;
        if (fds[0].revents & POLLIN)
            break;
        const int64_t now = cld::now_ms();
        events.now = now;
        provider->dispatchEvents(events);
        service.after_poll(fds, first, now);
        port.dispatch(now);
    }
    service.shutdown(cld::now_ms());
    port.dispatch(cld::now_ms());
    for (int64_t until = cld::now_ms() + kStopMs; port.stopping() && cld::now_ms() < until;) {
        usleep(kDispatchMs * 1000);
        events.now = cld::now_ms();
        provider->dispatchEvents(events);
        port.dispatch(events.now);
    }
    framework.disconnect();
    log_info("stopped");
    _exit(0);
}

}  // namespace crs
