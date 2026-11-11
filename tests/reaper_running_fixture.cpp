#include <iostream>
#include <string>

int main() {
    // A controlled process name for discovery tests, not a Cockos host.
    std::cout << "READY" << std::endl;
    std::string command;
    std::getline(std::cin, command);
    return command == "quit" ? 0 : 1;
}
