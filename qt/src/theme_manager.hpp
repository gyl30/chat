#ifndef CHAT_QT_SRC_THEME_MANAGER_HPP
#define CHAT_QT_SRC_THEME_MANAGER_HPP

#include <QColor>
#include <QHash>
#include <QList>
#include <QObject>
#include <QPalette>
#include <QString>
#include <QTemporaryDir>
#include <memory>
#include "chat_background.hpp"

enum class chat_appearance
{
    system,
    light,
    dark
};

// QSS and custom painters use the same semantic colors from external theme files.
class theme_manager final : public QObject
{
    Q_OBJECT
   public:
    static theme_manager& instance();
    chat_appearance appearance() const noexcept { return appearance_; }
    void set_appearance(chat_appearance appearance);
    chat_background background() const noexcept { return background_; }
    bool is_dark() const noexcept { return dark_; }
    void set_background(chat_background choice);
    void load_settings();
    bool reload();
    bool valid() const noexcept { return !colors_.isEmpty(); }
    QString const& last_error() const noexcept { return error_; }
    QColor color(QString const& role) const;
    QColor on_accent() const { return color(QStringLiteral("on_accent")); }
    QColor avatar_text() const { return color(QStringLiteral("avatar_text")); }
    QColor highlight() const { return color(QStringLiteral("highlight")); }
    QColor avatar_background(quint64 seed) const;
    QString style_sheet() const { return sheet_; }
    QString style_sheet(QString text) const;
   signals:
    void changed();
    void background_changed();
    void load_failed(QString message);

   private:
    theme_manager();
    bool apply(chat_appearance appearance);
    void save_settings() const;
    chat_appearance appearance_ = chat_appearance::system;
    chat_background background_ = chat_background::solid;
    bool dark_ = false;
    bool system_dark_ = false;
    QHash<QString, QColor> colors_;
    QList<QColor> avatars_;
    QString sheet_;
    QString error_;
    std::unique_ptr<QTemporaryDir> icon_directory_;
    QPalette original_palette_;
};
QColor themed(char const* role);
#endif
