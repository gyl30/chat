#ifndef CHAT_QT_SRC_USER_DELEGATE_HPP
#define CHAT_QT_SRC_USER_DELEGATE_HPP

#include <QStyledItemDelegate>
#include <functional>

class user_delegate : public QStyledItemDelegate
{
    Q_OBJECT
   public:
    enum class layout
    {
        compact,
        member,
        picker
    };
    static constexpr int badge_role = Qt::UserRole + 20;
    static constexpr int chosen_role = Qt::UserRole + 21;
    explicit user_delegate(QObject* parent = nullptr, std::function<QString(QModelIndex const&)> status = {}, layout mode = layout::compact);
    void paint(QPainter* painter, QStyleOptionViewItem const& option, QModelIndex const& index) const override;
    QSize sizeHint(QStyleOptionViewItem const& option, QModelIndex const& index) const override;
    bool editorEvent(QEvent* event, QAbstractItemModel* model, QStyleOptionViewItem const& option, QModelIndex const& index) override;

   private:
    std::function<QString(QModelIndex const&)> status_;
    layout mode_;

   signals:
    void avatar_clicked(QModelIndex index);
    void body_clicked(QModelIndex index);
};

#endif
