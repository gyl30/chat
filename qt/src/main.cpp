#include <utility>

#include <QApplication>
#include <QString>

#include "main_window.hpp"
#include "theme_manager.hpp"

int main(int argc, char* argv[])
{
    QApplication application(argc, argv);
    // Settings identity; also enables remembering recently signed-in accounts.
    QApplication::setOrganizationName(QStringLiteral("chat"));
    QApplication::setApplicationName(QStringLiteral("chat_qt"));
    theme_manager::instance().load_settings();

    QString server_url = QStringLiteral("ws://127.0.0.1:18080/ws");
    if (argc > 1)
    {
        server_url = QString::fromLocal8Bit(argv[1]);
    }

    main_window window(std::move(server_url));
    window.show();
    return application.exec();
}
