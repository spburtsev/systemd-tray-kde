#include <QAction>
#include <QApplication>
#include <QByteArray>
#include <QCommandLineParser>
#include <QIcon>
#include <QMenu>
#include <QPointer>
#include <QProcess>
#include <QSocketNotifier>
#include <QStyle>
#include <QSystemTrayIcon>
#include <QTimer>

#include <systemd/sd-bus.h>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <poll.h>
#include <time.h>
#include <utility>

static QIcon themed_icon(const QStringList &names)
{
    for (const auto &name : names) {
        auto icon = QIcon::fromTheme(name);
        if (!icon.isNull()) {
            return icon;
        }
    }

    return QApplication::style()->standardIcon(QStyle::SP_ComputerIcon);
}

static const char *get_terminal()
{
    return "konsole";
}

enum class ServiceStatus {
    Active,
    Inactive,
    Failed,
};

static const char *to_string(ServiceStatus status)
{
    switch (status) {
    case ServiceStatus::Active:
        return "active";
    case ServiceStatus::Inactive:
        return "inactive";
    case ServiceStatus::Failed:
        return "failed";
    default:
        return "unknown";
    }
}

struct SdBusDeleter {
    void operator()(sd_bus *bus) const
    {
        sd_bus_flush_close_unref(bus);
    }
};

struct SdBusSlotDeleter {
    void operator()(sd_bus_slot *slot) const
    {
        sd_bus_slot_unref(slot);
    }
};

struct SdBusMessageDeleter {
    void operator()(sd_bus_message *message) const
    {
        sd_bus_message_unref(message);
    }
};

using SdBusPtr = std::unique_ptr<sd_bus, SdBusDeleter>;
using SdBusSlotPtr = std::unique_ptr<sd_bus_slot, SdBusSlotDeleter>;
using SdBusMessagePtr = std::unique_ptr<sd_bus_message, SdBusMessageDeleter>;

struct ServiceTray {
    QString service;
    QString label;

    QMenu menu;
    QSystemTrayIcon tray;
    QTimer timer;

    SdBusPtr bus;
    SdBusSlotPtr properties_match;
    QSocketNotifier bus_read_notifier;
    QSocketNotifier bus_write_notifier;
    QTimer bus_timeout;

    QPointer<QAction> status_action;
    QPointer<QAction> start_action;
    QPointer<QAction> stop_action;
    QPointer<QAction> restart_action;
    QPointer<QAction> logs_action;
    QPointer<QAction> refresh_action;
    QPointer<QAction> quit_action;
    QString unit_path;
    QString active_state;
    QString sub_state;
    bool user_service = false;
    bool status_request_pending = false;
    bool status_refresh_requested = false;

    ServiceTray(QString service_name, QString display_label, bool is_user_service)
        : service(std::move(service_name)),
          label(std::move(display_label)),
          menu(QMenu()),
          tray(QSystemTrayIcon()),
          timer(QTimer()),
          bus_read_notifier(QSocketNotifier::Read),
          bus_write_notifier(QSocketNotifier::Write),
          bus_timeout(QTimer()),
          user_service(is_user_service)
    {
        this->status_action = this->menu.addAction("Status: unknown");
        this->status_action->setEnabled(false);

        this->start_action = this->menu.addAction("Start");
        this->stop_action = this->menu.addAction("Stop");
        this->restart_action = this->menu.addAction("Restart");

        this->menu.addSeparator();

        this->logs_action = this->menu.addAction("Follow logs");
        this->refresh_action = this->menu.addAction("Refresh");

        this->menu.addSeparator();

        this->quit_action = this->menu.addAction("Quit indicator");

        this->tray.setContextMenu(&this->menu);

        QObject::connect(this->start_action.data(), &QAction::triggered, [this] {
            this->service_action("StartUnit");
        });

        QObject::connect(this->stop_action.data(), &QAction::triggered, [this] {
            this->service_action("StopUnit");
        });

        QObject::connect(this->restart_action.data(), &QAction::triggered, [this] {
            this->service_action("RestartUnit");
        });

        QObject::connect(this->logs_action.data(), &QAction::triggered, [this] {
            this->open_logs();
        });

        QObject::connect(this->refresh_action.data(), &QAction::triggered, [this] {
            this->update();
        });

        QObject::connect(this->quit_action.data(), &QAction::triggered, qApp, &QApplication::quit);

        QObject::connect(&this->tray, &QSystemTrayIcon::activated,
                         [this](QSystemTrayIcon::ActivationReason reason) {
                             if (reason == QSystemTrayIcon::Trigger) {
                                 this->update();
                                 this->tray.showMessage(
                                     this->label,
                                     this->tray.toolTip(),
                                     QSystemTrayIcon::Information,
                                     2000);
                             }
                         });

        QObject::connect(&this->timer, &QTimer::timeout, [this] {
            this->update();
        });

        QObject::connect(&this->bus_read_notifier, &QSocketNotifier::activated,
                         [this] {
                             this->process_bus_events();
                         });

        QObject::connect(&this->bus_write_notifier, &QSocketNotifier::activated,
                         [this] {
                             this->process_bus_events();
                         });

        this->bus_timeout.setSingleShot(true);
        QObject::connect(&this->bus_timeout, &QTimer::timeout, [this] {
            this->process_bus_events();
        });
    }

    ServiceTray(const ServiceTray &) = delete;
    ServiceTray &operator=(const ServiceTray &) = delete;
    ServiceTray(ServiceTray &&) = delete;
    ServiceTray &operator=(ServiceTray &&) = delete;

    bool setup_systemd_connection()
    {
        if (this->bus) {
            return true;
        }

        sd_bus *raw_bus = nullptr;
        const int result = this->user_service
                               ? sd_bus_open_user(&raw_bus)
                               : sd_bus_open_system(&raw_bus);

        if (result < 0) {
            this->set_unavailable("D-Bus connection failed: " + error_string(result));
            return false;
        }

        this->bus.reset(raw_bus);
        this->configure_bus_watchers();

        int call_result = sd_bus_call_method_async(
            this->bus.get(),
            nullptr,
            "org.freedesktop.systemd1",
            "/org/freedesktop/systemd1",
            "org.freedesktop.systemd1.Manager",
            "Subscribe",
            &ServiceTray::handle_subscribe_reply,
            this,
            "");

        if (call_result < 0) {
            this->disconnect_systemd("Could not subscribe to systemd: " + error_string(call_result));
            return false;
        }

        const QByteArray service_name = this->service.toUtf8();
        call_result = sd_bus_call_method_async(
            this->bus.get(),
            nullptr,
            "org.freedesktop.systemd1",
            "/org/freedesktop/systemd1",
            "org.freedesktop.systemd1.Manager",
            "LoadUnit",
            &ServiceTray::handle_load_unit_reply,
            this,
            "s",
            service_name.constData());

        if (call_result < 0) {
            this->disconnect_systemd("Could not load unit: " + error_string(call_result));
            return false;
        }

        this->configure_bus_watchers();
        return true;
    }

    void start(int interval_ms)
    {
        this->timer.start(interval_ms);
        this->update();
        this->tray.show();
    }

private:
    static QString error_string(int result)
    {
        return QString::fromLocal8Bit(std::strerror(-result));
    }

    static QString message_error(sd_bus_message *message)
    {
        const sd_bus_error *error = sd_bus_message_get_error(message);

        if (error == nullptr) {
            return "Unknown D-Bus error";
        }
        if (error->message != nullptr) {
            return QString::fromUtf8(error->message);
        }
        if (error->name != nullptr) {
            return QString::fromUtf8(error->name);
        }
        return "Unknown D-Bus error";
    }

    static int handle_subscribe_reply(sd_bus_message *message, void *, sd_bus_error *)
    {
        if (sd_bus_message_is_method_error(message, nullptr) > 0) {
            qWarning("Could not subscribe to systemd signals: %s",
                     qUtf8Printable(message_error(message)));
        }
        return 0;
    }

    static int handle_load_unit_reply(sd_bus_message *message, void *userdata, sd_bus_error *)
    {
        auto *self = static_cast<ServiceTray *>(userdata);

        if (sd_bus_message_is_method_error(message, nullptr) > 0) {
            self->set_unavailable("Could not load unit: " + message_error(message));
            return 0;
        }

        const char *unit_path = nullptr;
        const int result = sd_bus_message_read(message, "o", &unit_path);
        if (result < 0 || unit_path == nullptr) {
            self->set_unavailable("Invalid response from systemd");
            return 0;
        }

        self->unit_path = QString::fromUtf8(unit_path);
        self->install_properties_match();
        self->request_status();
        return 0;
    }

    static int handle_properties_changed(sd_bus_message *message, void *userdata, sd_bus_error *)
    {
        auto *self = static_cast<ServiceTray *>(userdata);
        const char *interface_name = nullptr;

        const int result = sd_bus_message_read(message, "s", &interface_name);
        if (result >= 0
            && interface_name != nullptr
            && std::strcmp(interface_name, "org.freedesktop.systemd1.Unit") == 0) {
            self->request_status();
        }

        return 0;
    }

    static int handle_status_reply(sd_bus_message *message, void *userdata, sd_bus_error *)
    {
        auto *self = static_cast<ServiceTray *>(userdata);
        self->status_request_pending = false;

        if (sd_bus_message_is_method_error(message, nullptr) > 0) {
            self->set_unavailable("Status query failed: " + message_error(message));
            self->request_deferred_status();
            return 0;
        }

        QString active_state;
        QString sub_state;
        const int result = read_unit_properties(message, active_state, sub_state);
        if (result < 0) {
            self->set_unavailable("Invalid status response: " + error_string(result));
            self->request_deferred_status();
            return 0;
        }

        self->active_state = std::move(active_state);
        self->sub_state = std::move(sub_state);
        self->render_status();
        self->request_deferred_status();
        return 0;
    }

    static int handle_action_reply(sd_bus_message *message, void *userdata, sd_bus_error *)
    {
        auto *self = static_cast<ServiceTray *>(userdata);

        if (sd_bus_message_is_method_error(message, nullptr) > 0) {
            const QString detail = message_error(message);
            qWarning("systemd action failed: %s", qUtf8Printable(detail));
            self->tray.showMessage(
                self->label,
                "systemd action failed: " + detail,
                QSystemTrayIcon::Critical,
                5000);
        }

        self->request_status();
        return 0;
    }

    static int read_unit_properties(sd_bus_message *message, QString &active_state, QString &sub_state)
    {
        int result = sd_bus_message_enter_container(message, 'a', "{sv}");
        if (result < 0) {
            return result;
        }

        while ((result = sd_bus_message_enter_container(message, 'e', "sv")) > 0) {
            const char *property_name = nullptr;
            result = sd_bus_message_read(message, "s", &property_name);
            if (result < 0) {
                return result;
            }

            if (property_name != nullptr
                && (std::strcmp(property_name, "ActiveState") == 0
                    || std::strcmp(property_name, "SubState") == 0)) {
                result = sd_bus_message_enter_container(message, 'v', "s");
                if (result < 0) {
                    return result;
                }

                const char *property_value = nullptr;
                result = sd_bus_message_read(message, "s", &property_value);
                if (result < 0) {
                    return result;
                }

                const QString value = QString::fromUtf8(property_value);
                if (std::strcmp(property_name, "ActiveState") == 0) {
                    active_state = value;
                } else {
                    sub_state = value;
                }

                result = sd_bus_message_exit_container(message);
            } else {
                result = sd_bus_message_skip(message, "v");
            }

            if (result < 0) {
                return result;
            }

            result = sd_bus_message_exit_container(message);
            if (result < 0) {
                return result;
            }
        }

        if (result < 0) {
            return result;
        }

        result = sd_bus_message_exit_container(message);
        if (result < 0) {
            return result;
        }

        if (active_state.isEmpty()) {
            return -ENODATA;
        }

        return 0;
    }

    void install_properties_match()
    {
        this->properties_match.reset();

        const QByteArray path = this->unit_path.toUtf8();
        sd_bus_slot *raw_slot = nullptr;
        const int result = sd_bus_match_signal_async(
            this->bus.get(),
            &raw_slot,
            "org.freedesktop.systemd1",
            path.constData(),
            "org.freedesktop.DBus.Properties",
            "PropertiesChanged",
            &ServiceTray::handle_properties_changed,
            nullptr,
            this);

        if (result < 0) {
            qWarning("Could not subscribe to unit property changes: %s",
                     qUtf8Printable(error_string(result)));
            return;
        }

        this->properties_match.reset(raw_slot);
    }

    void request_status()
    {
        if (!this->bus || this->unit_path.isEmpty()) {
            return;
        }
        if (this->status_request_pending) {
            this->status_refresh_requested = true;
            return;
        }

        const QByteArray path = this->unit_path.toUtf8();
        const int result = sd_bus_call_method_async(
            this->bus.get(),
            nullptr,
            "org.freedesktop.systemd1",
            path.constData(),
            "org.freedesktop.DBus.Properties",
            "GetAll",
            &ServiceTray::handle_status_reply,
            this,
            "s",
            "org.freedesktop.systemd1.Unit");

        if (result < 0) {
            this->set_unavailable("Could not request status: " + error_string(result));
            return;
        }

        this->status_request_pending = true;
    }

    void request_deferred_status()
    {
        if (!this->status_refresh_requested) {
            return;
        }

        this->status_refresh_requested = false;
        this->request_status();
    }

    void update()
    {
        if (!this->bus) {
            this->setup_systemd_connection();
            return;
        }

        this->request_status();
        this->configure_bus_watchers();
    }

    void render_status()
    {
        ServiceStatus status = ServiceStatus::Inactive;
        if (this->active_state == "active") {
            status = ServiceStatus::Active;
        } else if (this->active_state == "failed") {
            status = ServiceStatus::Failed;
        }

        if (status == ServiceStatus::Active) {
            this->tray.setIcon(themed_icon({"emblem-default", "dialog-ok-apply", "media-playback-start"}));
        } else if (status == ServiceStatus::Failed) {
            this->tray.setIcon(themed_icon({"dialog-error", "emblem-error"}));
        } else {
            this->tray.setIcon(themed_icon({"process-stop", "dialog-warning", "emblem-warning"}));
        }

        const QString status_text = this->active_state.isEmpty()
                                        ? QString::fromUtf8(to_string(status))
                                        : this->active_state;
        QString tooltip = this->label + ": " + status_text;

        if (!this->sub_state.isEmpty()) {
            tooltip += " (" + this->sub_state + ")";
        }

        this->tray.setToolTip(tooltip);
        this->status_action->setText(
            "Status: " + status_text
            + (this->sub_state.isEmpty() ? "" : " / " + this->sub_state));

        this->start_action->setEnabled(status != ServiceStatus::Active);
        this->stop_action->setEnabled(status == ServiceStatus::Active);
        this->restart_action->setEnabled(status == ServiceStatus::Active);
    }

    void service_action(const char *action)
    {
        if (!this->bus) {
            this->set_unavailable("Not connected to systemd");
            return;
        }

        sd_bus_message *raw_message = nullptr;
        int result = sd_bus_message_new_method_call(
            this->bus.get(),
            &raw_message,
            "org.freedesktop.systemd1",
            "/org/freedesktop/systemd1",
            "org.freedesktop.systemd1.Manager",
            action);
        if (result < 0) {
            this->set_unavailable("Could not create action request: " + error_string(result));
            return;
        }

        SdBusMessagePtr message(raw_message);
        const QByteArray service_name = this->service.toUtf8();
        result = sd_bus_message_append(
            message.get(),
            "ss",
            service_name.constData(),
            "replace");
        if (result >= 0) {
            result = sd_bus_message_set_allow_interactive_authorization(message.get(), true);
        }
        if (result >= 0) {
            result = sd_bus_call_async(
                this->bus.get(),
                nullptr,
                message.get(),
                &ServiceTray::handle_action_reply,
                this,
                0);
        }

        if (result < 0) {
            this->set_unavailable("Could not send action request: " + error_string(result));
            return;
        }

        this->configure_bus_watchers();
    }

    void process_bus_events()
    {
        if (!this->bus) {
            return;
        }

        int result = 0;
        do {
            result = sd_bus_process(this->bus.get(), nullptr);
        } while (result > 0);

        if (result < 0) {
            this->disconnect_systemd("D-Bus connection lost: " + error_string(result));
            return;
        }

        this->configure_bus_watchers();
    }

    void configure_bus_watchers()
    {
        if (!this->bus) {
            this->bus_read_notifier.setEnabled(false);
            this->bus_write_notifier.setEnabled(false);
            this->bus_timeout.stop();
            return;
        }

        const int descriptor = sd_bus_get_fd(this->bus.get());
        const int events = sd_bus_get_events(this->bus.get());
        uint64_t timeout_usec = UINT64_MAX;
        const int timeout_result = sd_bus_get_timeout(this->bus.get(), &timeout_usec);

        if (descriptor < 0 || events < 0 || timeout_result < 0) {
            const int result = std::min({descriptor, events, timeout_result});
            this->disconnect_systemd("Could not monitor D-Bus: " + error_string(result));
            return;
        }

        this->bus_read_notifier.setEnabled(false);
        this->bus_write_notifier.setEnabled(false);
        this->bus_read_notifier.setSocket(descriptor);
        this->bus_write_notifier.setSocket(descriptor);
        this->bus_read_notifier.setEnabled((events & POLLIN) != 0);
        this->bus_write_notifier.setEnabled((events & POLLOUT) != 0);

        if (timeout_usec == UINT64_MAX) {
            this->bus_timeout.stop();
            return;
        }

        timespec now{};
        if (clock_gettime(CLOCK_MONOTONIC, &now) < 0) {
            this->disconnect_systemd(
                "Could not read monotonic clock: " + QString::fromLocal8Bit(std::strerror(errno)));
            return;
        }

        const uint64_t now_usec = static_cast<uint64_t>(now.tv_sec) * 1'000'000
                                  + static_cast<uint64_t>(now.tv_nsec) / 1'000;
        const uint64_t remaining_usec = timeout_usec > now_usec ? timeout_usec - now_usec : 0;
        const uint64_t timeout_ms = remaining_usec / 1'000
                                    + (remaining_usec % 1'000 != 0 ? 1 : 0);
        const int bounded_timeout_ms = static_cast<int>(std::min<uint64_t>(
            timeout_ms,
            static_cast<uint64_t>(std::numeric_limits<int>::max())));
        this->bus_timeout.start(bounded_timeout_ms);
    }

    void disconnect_systemd(const QString &reason)
    {
        this->bus_read_notifier.setEnabled(false);
        this->bus_write_notifier.setEnabled(false);
        this->bus_timeout.stop();
        this->properties_match.reset();
        this->bus.reset();
        this->unit_path.clear();
        this->active_state.clear();
        this->sub_state.clear();
        this->status_request_pending = false;
        this->status_refresh_requested = false;
        this->set_unavailable(reason);
    }

    void set_unavailable(const QString &reason)
    {
        qWarning("%s", qUtf8Printable(reason));
        this->tray.setIcon(themed_icon({"dialog-error", "emblem-error"}));
        this->tray.setToolTip(this->label + ": unavailable");
        this->status_action->setText("Status: unavailable");
        this->start_action->setEnabled(false);
        this->stop_action->setEnabled(false);
        this->restart_action->setEnabled(false);
    }

    void open_logs()
    {
        if (this->user_service) {
            QProcess::startDetached(
                get_terminal(),
                {"--hold", "-e", "journalctl", "--user-unit", this->service, "-n", "100", "-f"});
        } else {
            QProcess::startDetached(
                get_terminal(),
                {"--hold", "-e", "journalctl", "-u", this->service, "-n", "100", "-f"});
        }
    }
};

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    QApplication::setQuitOnLastWindowClosed(false);

    QCommandLineParser parser;
    parser.setApplicationDescription("KDE/Qt tray indicator for a systemd service");
    parser.addHelpOption();

    parser.addPositionalArgument("service", "systemd unit, e.g. postgresql.service");

    QCommandLineOption label_option({"l", "label"}, "Display label", "label");
    QCommandLineOption user_option({"u", "user"}, "Monitor a user service instead of a system service");
    QCommandLineOption interval_option({"i", "interval"}, "Refresh interval in seconds", "seconds", "5");

    parser.addOption(label_option);
    parser.addOption(user_option);
    parser.addOption(interval_option);

    parser.process(app);

    const QStringList positional = parser.positionalArguments();

    if (positional.isEmpty()) {
        parser.showHelp(1);
    }

    if (!QSystemTrayIcon::isSystemTrayAvailable()) {
        qCritical("No system tray is available.");
        return 1;
    }
    bool interval_parsed = false;
    const int interval_sec = parser.value(interval_option).toInt(&interval_parsed);
    if (!interval_parsed
        || interval_sec <= 0
        || interval_sec > std::numeric_limits<int>::max() / 1000) {
        qCritical("Invalid interval value: %s", qUtf8Printable(parser.value(interval_option)));
        return 1;
    }
    const int interval_ms = interval_sec * 1000;

    const QString service = positional.first();
    const QString label = parser.value(label_option).isEmpty()
                              ? QString(service).remove(".service")
                              : parser.value(label_option);

    const bool user_service = parser.isSet(user_option);

    ServiceTray tray{service, label, user_service};
    if (!tray.setup_systemd_connection()) {
        return 1;
    }
    tray.start(interval_ms);
    return app.exec();
}
