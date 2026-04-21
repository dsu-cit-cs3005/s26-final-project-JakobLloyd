#include "Arena.h"
#include <iostream>

int main(int argc, char* argv[])
{
    if (argc != 2)
    {
        std::cerr << "Usage: " << argv[0] << " <config_file>\n";
        return 1;
    }

    Arena arena;
    if (!arena.load_config(argv[1]))
        return 1;

    arena.load_robots();
    arena.run();

    return 0;
}
