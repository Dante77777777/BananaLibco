#include "thread.h"
#include <iostream>
#include <memory>
#include <vector>
#include <unistd.h>  

using namespace banana;

void func()
{
    std::cout << "id: " << Thread::GetThreadId() << ", name: " << Thread::GetName();
    std::cout << ", this id: " << Thread::GetThis()->getId() << ", this name: " << Thread::GetThis()->getName() << std::endl;
    sleep(60);
}


int main()
{
    std::vector<std::shared_ptr<Thread>> threads;
    for(int i = 0; i < 10; i++){
        std::shared_ptr<Thread> thread = std::make_shared<Thread>(&func,"Thread ID:" + std::to_string(i));
        threads.push_back(thread);
    }
    for(int i = 0; i < 10; i++){
        threads[i]->join();
    }
    return 0;
}