#include "commands.h"
namespace pi5 {
static bool Build(const Draw &d,uint32_t base,Commands &out,std::string &error,bool clear){
    out={};error.clear();out.bytes.resize(256*1024);
    EncodedCommands encoded;const char *why=nullptr;
    bool ok=clear?EncodeClear(d,base,out.bytes.data(),static_cast<uint32_t>(out.bytes.size()),encoded,why):EncodeDraw(d,base,out.bytes.data(),static_cast<uint32_t>(out.bytes.size()),encoded,why);
    if(!ok){error=why?why:"command encoding failed";out={};return false;}
    out.bytes.resize(encoded.bytes);out.binStart=encoded.binStart;out.binEnd=encoded.binEnd;out.renderStart=encoded.renderStart;out.renderEnd=encoded.renderEnd;out.tileBytes=encoded.tileBytes;out.stateBytes=encoded.stateBytes;return true;
}
bool BuildDraw(const Draw &d,uint32_t base,Commands &out,std::string &error){return Build(d,base,out,error,false);}
bool BuildClear(const Draw &d,uint32_t base,Commands &out,std::string &error){return Build(d,base,out,error,true);}
}
