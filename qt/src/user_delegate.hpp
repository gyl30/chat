#ifndef CHAT_QT_SRC_USER_DELEGATE_HPP
#define CHAT_QT_SRC_USER_DELEGATE_HPP

#include <QStyledItemDelegate>

class user_delegate final : public QStyledItemDelegate
{
   public:
    explicit user_delegate(QObject* parent = nullptr);

    void paint(QPainter* painter, QStyleOptionViewItem const& option, QModelIndex const& index) const override;
    QSize sizeHint(QStyleOptionViewItem const& option, QModelIndex const& index) const override;
};

#endif
