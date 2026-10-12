#include <utility>

#include <QApplication>
#include <QString>
#include <QMessageBox>

#include "main_window.hpp"
#include "theme_manager.hpp"

int main(int argc, char* argv[])
{
    QApplication application(argc, argv);
    // Settings identity; also enables remembering recently signed-in accounts.
    QApplication::setOrganizationName(QStringLiteral("chat"));
    QApplication::setApplicationName(QStringLiteral("chat_qt"));
    auto& themes = theme_manager::instance();
    themes.load_settings();
    if (!themes.valid())
    {
        QMessageBox::critical(nullptr, QStringLiteral("无法加载主题"), themes.last_error());
        return 1;
    }

    QString server_url;
    if (argc > 1)
    {
        server_url = QString::fromLocal8Bit(argv[1]);
    }

    main_window window(std::move(server_url));
    window.show();
    auto report_theme_error = [&window](QString const& message) {
        auto* error = new QMessageBox(QMessageBox::Warning, QStringLiteral("主题文件错误"),
            message + QStringLiteral("\n应用继续使用完整的有效主题。"), QMessageBox::Ok, &window);
        error->setAttribute(Qt::WA_DeleteOnClose);
        error->open();
    };
    QObject::connect(&themes, &theme_manager::load_failed, &window, report_theme_error);
    if (!themes.last_error().isEmpty()) { report_theme_error(themes.last_error()); }
    return application.exec();
}
