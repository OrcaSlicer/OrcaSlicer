#include <chrono>
#include <iostream>
#include <thread>

int main()
{
    // Bound the fixture lifetime so a leaked pipe cannot hang the test suite indefinitely.
    std::cout << "ready" << std::endl;
    std::cerr << "ready" << std::endl;
    std::this_thread::sleep_for(std::chrono::seconds(3));
    return 0;
}
