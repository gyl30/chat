#ifndef CHAT_QT_SRC_THEME_MANAGER_HPP
#define CHAT_QT_SRC_THEME_MANAGER_HPP

#include <QColor>
#include <QHash>
#include <QList>
#include <QObject>
#include <QPalette>
#include <QString>

enum class chat_theme_id
{
    classic,
    paper,
    slate,
    ink,
    terminal,
};

enum class chat_appearance
{
    system,
    light,
    dark,
};

struct chat_theme_info
{
    chat_theme_id id;
    QString key;
    QString name;
};

// The stylesheet and painters are written against the classic palette. Every other theme, and the
// single night theme, is produced by mapping each classic color to its semantic role.
class theme_manager final : public QObject
{
    Q_OBJECT

   public:
    static theme_manager& instance();
    static QList<chat_theme_info> const& themes();

    chat_theme_id theme() const noexcept { return theme_; }
    chat_appearance appearance() const noexcept { return appearance_; }
    // Night mode replaces the chosen theme entirely; the chosen theme returns in light mode.
    bool dark() const;
    void set_theme(chat_theme_id theme);
    void set_appearance(chat_appearance appearance);
    void load_settings();

    QColor color(QColor const& classic) const;
    QColor on_accent() const;
    QColor avatar_text() const;
    // Unread badges and similar marks: the theme's second color (copper, red, amber).
    QColor highlight() const;
    QColor avatar_background(QColor const& classic) const;
    QString style_sheet(QString classic) const;

   signals:
    void changed();

   private:
    theme_manager();
    void rebuild();
    void save_settings() const;

    chat_theme_id theme_ = chat_theme_id::classic;
    chat_appearance appearance_ = chat_appearance::system;
    bool system_dark_ = false;
    bool identity_ = true;
    QHash<QRgb, QColor> colors_;
    QColor on_accent_;
    QColor avatar_text_;
    QColor avatar_ground_;
    QColor highlight_;
    QPalette original_palette_;
};

// Maps a color of the classic palette to the active theme.
QColor themed(QColor const& classic);
QColor themed(char const* classic);

#endif
