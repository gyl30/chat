#ifndef CHAT_QT_SRC_THEME_HPP
#define CHAT_QT_SRC_THEME_HPP

#include <QLabel>
#include <QSize>
#include <QString>

class QWidget;

class feedback_label final : public QLabel
{
    Q_OBJECT
   public:
    explicit feedback_label(QWidget* parent = nullptr);
    void show_error(QString message);

   private:
    QObject notification_;
};

namespace chat_theme
{

inline constexpr int window_resize_border = 5;
inline constexpr QSize login_window_size{380, 540};
inline constexpr int auth_card_width = login_window_size.width() - 2 * window_resize_border;
inline constexpr int auth_padding = 36;
inline constexpr int auth_spacing = 12;
inline constexpr int login_avatar_size = 88;

inline constexpr int dialog_padding = 24;
inline constexpr int dialog_spacing = 12;
inline constexpr int dialog_small_width = 480;
inline constexpr int dialog_normal_width = 520;

inline constexpr int dialog_row_height = 62;
inline constexpr int dialog_avatar_size = 46;
inline constexpr int dialog_left = 10;
inline constexpr int dialog_avatar_top = 8;
inline constexpr int dialog_text_left = 68;
inline constexpr int dialog_name_top = 10;
inline constexpr int dialog_preview_top = 34;
inline constexpr int dialog_right = 10;
inline constexpr int dialog_unread_height = 19;

inline constexpr int message_max_width = 430;
inline constexpr int message_avatar_size = 33;
inline constexpr int message_avatar_skip = 40;
inline constexpr int message_side_margin = 16;
inline constexpr int message_padding_horizontal = 11;
inline constexpr int message_padding_vertical = 8;
inline constexpr int message_margin_top = 6;
inline constexpr int message_margin_top_attached = 0;
inline constexpr int message_margin_bottom = 2;
inline constexpr int message_radius = 16;
inline constexpr int message_attached_radius = 5;
inline constexpr int message_tail_width = 6;
inline constexpr int message_tail_height = 9;
inline constexpr int message_time_gap = 8;
inline constexpr int message_name_gap = 3;
inline constexpr int message_date_height = 24;
inline constexpr int message_date_margin_top = 10;
inline constexpr int message_date_margin_bottom = 6;

inline constexpr int compose_height = 56;
inline constexpr int compose_button_width = 44;
inline constexpr int compose_button_height = 46;
inline constexpr int compose_field_min_height = 36;

}    // namespace chat_theme

QString chat_style_sheet();
bool confirm_action(QWidget* parent, QString const& title, QString const& text, QString const& action);

#endif
