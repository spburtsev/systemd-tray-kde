#include <QAction>
#include <QApplication>
#include <QCommandLineParser>
#include <QIcon>
#include <QMenu>
#include <QPointer>
#include <QProcess>
#include <QStyle>
#include <QSystemTrayIcon>
#include <QTimer>

#include <memory>
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

struct ServiceTray {
    QString service;
    QString label;

    QMenu menu;
    QSystemTrayIcon tray;
    QTimer timer;

    QPointer<QAction> status_action;
    QPointer<QAction> start_action;
    QPointer<QAction> stop_action;
    QPointer<QAction> restart_action;
    QPointer<QAction> logs_action;
    QPointer<QAction> refresh_action;
    QPointer<QAction> quit_action;
    bool user_service;

    ServiceTray(QString service, QString label, bool user_service)
        : service(std::move(service)),
          label(std::move(label)),
          user_service(user_service),
          menu(QMenu()),
          tray(QSystemTrayIcon()),
          timer(QTimer())
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
            this->service_action("start");
        });

        QObject::connect(this->stop_action.data(), &QAction::triggered, [this] {
            this->service_action("stop");
        });

        QObject::connect(this->restart_action.data(), &QAction::triggered, [this] {
            this->service_action("restart");
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
    }

    void start(int interval_ms)
    {
        this->timer.start(interval_ms);
        this->update();
        this->tray.show();
    }

private:
    QStringList systemctl_args(const QStringList &args) const
    {
        QStringList result;

        if (this->user_service) {
            result << "--user";
        }

        result << args;
        return result;
    }

    int run_quiet(const QString &program, const QStringList &args) const
    {
        QProcess process;
        process.start(program, args);
        process.waitForFinished(3000);
        return process.exitCode();
    }

    QString run_capture(const QString &program, const QStringList &args) const
    {
        QProcess process;
        process.start(program, args);
        process.waitForFinished(3000);

        QString out = QString::fromUtf8(process.readAllStandardOutput()).trimmed();
        QString err = QString::fromUtf8(process.readAllStandardError()).trimmed();

        return !out.isEmpty() ? out : err;
    }

    ServiceStatus service_status() const
    {
        if (this->run_quiet("systemctl", this->systemctl_args({"is-active", "--quiet", this->service})) == 0) {
            return ServiceStatus::Active;
        }

        if (this->run_quiet("systemctl", this->systemctl_args({"is-failed", "--quiet", this->service})) == 0) {
            return ServiceStatus::Failed;
        }

        return ServiceStatus::Inactive;
    }

    void update()
    {
        const ServiceStatus status = this->service_status();

        if (status == ServiceStatus::Active) {
            this->tray.setIcon(themed_icon({"emblem-default", "dialog-ok-apply", "media-playback-start"}));
        } else if (status == ServiceStatus::Failed) {
            this->tray.setIcon(themed_icon({"dialog-error", "emblem-error"}));
        } else {
            this->tray.setIcon(themed_icon({"process-stop", "dialog-warning", "emblem-warning"}));
        }

        const QString substate = this->run_capture(
            "systemctl",
            this->systemctl_args({"show", this->service, "-p", "SubState", "--value"}));

        QString tooltip = this->label + ": " + to_string(status);

        if (!substate.isEmpty()) {
            tooltip += " (" + substate + ")";
        }

        this->tray.setToolTip(tooltip);
        this->status_action->setText("Status: " + QString(to_string(status)) + (substate.isEmpty() ? "" : " / " + substate));

        this->start_action->setEnabled(status != ServiceStatus::Active);
        this->stop_action->setEnabled(status == ServiceStatus::Active);
        this->restart_action->setEnabled(status == ServiceStatus::Active);
    }

    void service_action(const char *action)
    {
        if (this->user_service) {
            QProcess::startDetached("systemctl", {"--user", action, this->service});
        } else {
            QProcess::startDetached("pkexec", {"systemctl", action, this->service});
        }

        QTimer::singleShot(1500, [this] {
            this->update();
        });
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
    if (!interval_parsed || interval_sec <= 0) {
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
    tray.start(interval_ms);
    return app.exec();
}
