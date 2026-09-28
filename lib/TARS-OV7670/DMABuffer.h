#pragma once
#include "rom/lldesc.h"
#include <stdlib.h>
#include "esp_heap_caps.h"

class DMABuffer{
public:
  lldesc_t descriptor;
  unsigned char* buffer;
  int len;

  DMABuffer(int bytes):buffer(nullptr),len(0){
    if(bytes<=0)return;
    buffer=(unsigned char*)heap_caps_malloc(bytes,MALLOC_CAP_DMA);
    if(!buffer)return;
    len=bytes;
    descriptor.length=bytes;
    descriptor.size=bytes;
    descriptor.owner=1;
    descriptor.sosf=0;
    descriptor.buf=buffer;
    descriptor.offset=0;
    descriptor.empty=0;
    descriptor.eof=1;
    descriptor.qe.stqe_next=nullptr;
  }

  ~DMABuffer(){
    if(buffer){
      heap_caps_free(buffer);
      buffer=nullptr;
    }
    len=0;
  }

  bool valid() const{
    return buffer!=nullptr&&len>0;
  }

  void next(DMABuffer*n){
    descriptor.qe.stqe_next=n?&(n->descriptor):nullptr;
  }

  int sampleCount() const{
    return len/4;
  }
};
