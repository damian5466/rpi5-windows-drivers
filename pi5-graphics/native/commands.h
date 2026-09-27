#pragma once
#include "encode.h"
#include <string>
#include <vector>
namespace pi5 {
struct Commands {
    std::vector<uint8_t> bytes;
    uint32_t binStart = 0, binEnd = 0, renderStart = 0, renderEnd = 0;
    uint32_t tileBytes = 0, stateBytes = 0;
};
bool BuildDraw(const Draw &draw, uint32_t commandAddress, Commands &out, std::string &error);
bool BuildClear(const Draw &surface, uint32_t commandAddress, Commands &out, std::string &error);
}
