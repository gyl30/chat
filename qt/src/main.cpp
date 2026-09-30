#include <utility>

#include <QApplication>
#include <QString>

#include "main_window.hpp"

int main(int argc, char* argv[])
{
    QApplication application(argc, argv);

    QString server_url = QStringLiteral("ws://127.0.0.1:18080/ws");
    if (argc > 1)
    {
        server_url = QString::fromLocal8Bit(argv[1]);
    }

    main_window window(std::move(server_url));
    window.show();
    return application.exec();
}
