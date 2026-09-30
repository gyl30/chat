#include <iostream>

#include "online_users.hpp"

class chat_session
{
};

int main()
{
    online_users users;
    chat_session first;
    chat_session second;

    if (!users.add(1, first))
    {
        std::cerr << "FAIL online user registration\n";
        return 1;
    }
    if (users.find(1) != &first || users.find(2) != nullptr)
    {
        std::cerr << "FAIL online user lookup\n";
        return 1;
    }
    std::cout << "PASS online user registration\n";
    std::cout << "PASS online user lookup\n";

    if (users.add(1, second))
    {
        std::cerr << "FAIL duplicate online user rejected\n";
        return 1;
    }
    std::cout << "PASS duplicate online user rejected\n";

    users.remove(1, second);
    if (users.add(1, second))
    {
        std::cerr << "FAIL online user release ownership\n";
        return 1;
    }
    std::cout << "PASS online user release ownership\n";

    users.remove(1, first);
    if (users.find(1) != nullptr || !users.add(1, second))
    {
        std::cerr << "FAIL online user reuse\n";
        return 1;
    }
    std::cout << "PASS online user reuse\n";

    return 0;
}
