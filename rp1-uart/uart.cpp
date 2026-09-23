// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <windows.h>
#include <cfgmgr32.h>
#include <initguid.h>
#include <devpkey.h>
#include <ntddser.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <stdexcept>
struct Handle {
 HANDLE value;
 explicit Handle(HANDLE v):value(v){}
 ~Handle(){if(value!=INVALID_HANDLE_VALUE && value)CloseHandle(value);}
 Handle(const Handle&)=delete;
 void close(){if(value!=INVALID_HANDLE_VALUE)CloseHandle(value);value=INVALID_HANDLE_VALUE;}
};
static void check(bool ok,const char* message){if(!ok){printf("FAIL: %s (Win32 %lu)\n",message,GetLastError());throw std::runtime_error(message);}}
static std::wstring port(const std::wstring& name){
 ULONG size=0;
 check(CM_Get_Device_Interface_List_SizeW(&size,(LPGUID)&GUID_DEVINTERFACE_COMPORT,nullptr,CM_GET_DEVICE_INTERFACE_LIST_PRESENT)==CR_SUCCESS,"interface list size");
 std::vector<wchar_t> list(size);
 check(CM_Get_Device_Interface_ListW((LPGUID)&GUID_DEVINTERFACE_COMPORT,nullptr,list.data(),size,CM_GET_DEVICE_INTERFACE_LIST_PRESENT)==CR_SUCCESS,"interface list");
 for(auto p=list.data();*p;p+=wcslen(p)+1){
  wchar_t friendly[128]={};DEVPROPTYPE type;ULONG bytes=sizeof(friendly);
  if(CM_Get_Device_Interface_PropertyW(p,&DEVPKEY_DeviceInterface_Serial_PortName,&type,(PBYTE)friendly,&bytes,0)==CR_SUCCESS){
   wprintf(L"Serial interface: %s\n",friendly);
   if(type==DEVPROP_TYPE_STRING && name==friendly)return p;
  }
 }
 if(name.empty())return {};
 throw std::runtime_error("UART interface missing");
}
static void configure(HANDLE h,DWORD baud,BYTE bits=8,BYTE parity=NOPARITY,BYTE stops=ONESTOPBIT){
 DCB d={};d.DCBlength=sizeof(d);check(GetCommState(h,&d)!=0,"GetCommState");
 d.BaudRate=baud;d.ByteSize=bits;d.Parity=parity;d.StopBits=stops;d.fBinary=TRUE;d.fParity=parity!=NOPARITY;
 d.fOutxCtsFlow=FALSE;d.fOutxDsrFlow=FALSE;d.fDtrControl=DTR_CONTROL_DISABLE;d.fRtsControl=RTS_CONTROL_DISABLE;
 d.fDsrSensitivity=FALSE;d.fOutX=FALSE;d.fInX=FALSE;d.fErrorChar=FALSE;d.fNull=FALSE;d.fAbortOnError=FALSE;
 check(SetCommState(h,&d)!=0,"SetCommState");
 DCB actual={};actual.DCBlength=sizeof(actual);check(GetCommState(h,&actual)!=0,"GetCommState after set");
 check(actual.BaudRate==baud && actual.ByteSize==bits && actual.Parity==parity && actual.StopBits==stops,"line setting readback");
}
static DWORD number(const wchar_t* text,DWORD max){
 wchar_t* end=nullptr;unsigned long value=wcstoul(text,&end,0);
 if(!*text||*end||*text==L'-'||value>max)throw std::runtime_error("Invalid number");
 return value;
}
int wmain(int argc,wchar_t**argv){
 try{
  if(argc<2){puts("Pi5UartTool list\nPi5UartTool send UART BAUD BYTE...\nPi5UartTool read UART BAUD COUNT [TIMEOUT_MS]\nUses 8N1 without flow control. Bytes accept decimal or 0x-prefixed hex.");return 2;}
  std::wstring command=argv[1];
  if(command==L"list"){(void)port(L"");return 0;}
  if(argc<5)throw std::runtime_error("Specify port, baud and data/count");
  DWORD baud=number(argv[3],1000000);if(baud<300)throw std::runtime_error("Baud must be 300..1000000");
  std::vector<BYTE> data;
  DWORD timeout=2000;
  if(command==L"read"){
   DWORD count=number(argv[4],65536);if(!count||argc>6)throw std::runtime_error("Specify a nonzero byte count");data.resize(count);
   if(argc==6){timeout=number(argv[5],60000);if(!timeout)throw std::runtime_error("Timeout must be 1..60000 milliseconds");}
  }else if(command==L"send"){
   for(int i=4;i<argc;++i)data.push_back(static_cast<BYTE>(number(argv[i],255)));
   if(data.size()>65536)throw std::runtime_error("Maximum 65536 bytes");
  }else throw std::runtime_error("Unknown command");
  auto path=port(argv[2]);Handle h(CreateFileW(path.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_EXISTING,0,nullptr));
  check(h.value!=INVALID_HANDLE_VALUE,"open UART");configure(h.value,baud);
  COMMTIMEOUTS t={};t.ReadTotalTimeoutConstant=timeout;t.WriteTotalTimeoutConstant=10000;
  check(SetCommTimeouts(h.value,&t)!=0,"SetCommTimeouts");DWORD n=0;
  if(command==L"send"){
   check(WriteFile(h.value,data.data(),static_cast<DWORD>(data.size()),&n,nullptr)!=0,"WriteFile");
   check(n==data.size(),"complete write");printf("Sent %lu bytes\n",n);
  }else{
   check(ReadFile(h.value,data.data(),static_cast<DWORD>(data.size()),&n,nullptr)!=0,"ReadFile");
   for(DWORD i=0;i<n;++i)printf("%02X%s",data[i],(i+1)%16==0||i+1==n?"\n":" ");
   printf("Received %lu bytes\n",n);
  }
  return 0;
 }catch(std::exception const& e){fprintf(stderr,"%s\n",e.what());return 1;}
}
