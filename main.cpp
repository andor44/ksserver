#include <iostream>
#include <ni/XnCppWrapper.h>
#include <ni/XnStatus.h>
#include <ni/XnMacros.h>
#include <Linux-x86/XnPlatformLinux-x86.h>
#include <zmq.hpp>
#include <stdint.h>
#include <assert.h>

#include "lz4.h"

const uint8_t DepthFrameRequest = 1;
const uint8_t ImageFrameRequest = 2;
const uint8_t DepthDimensionRequest = 3;
const uint8_t ImageDimensionRequest = 4;
const uint8_t DepthBppRequest = 5;
const uint8_t ImageBppRequest = 6;

xn::ScriptNode sn;
xn::Context ctx;
xn::DepthGenerator depthGen;
xn::ImageGenerator imgGen;
xn::DepthMetaData depthMeta;
xn::ImageMetaData imageMeta;

zmq::message_t handleDetailRequest(const zmq::message_t& req)
{
  assert(req.size() == 1);
  
  uint8_t* data = (uint8_t*)req.data();
  assert(data != NULL);
  
  switch(*data)
  {
    case DepthFrameRequest:
      zmq::message_t(depthMeta.DataSize());
      break;
      
    default:
      break;
  }
}

#define COMPR_TYPE_NONE 1
#define COMPR_TYPE_LZ4 2

#define IMG_TYPE_DEPTH 1
#define IMG_TYPE_IMAGE 2
#define IMG_TYPE_IR 3

#include <time.h>
#include <sys/timeb.h>
// needs -lrt (real-time lib)
// 1970-01-01 epoch UTC time, 1 mcs resolution (divide by 1M to get time_t)
uint64_t ClockGetTime()
{
    timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000000LL + (uint64_t)ts.tv_nsec / 1000LL;
}

int main(int argc, char **argv) {

  // TODO: doesn't work on linux with kinect, node not found :(
  //xn::IRGenerator irGen;
  //xn::IRMetaData irMeta;
  
  XnStatus status;
  xn::EnumerationErrors err;
  
  status = ctx.InitFromXmlFile("/usr/share/openni/SamplesConfig.xml", sn, &err);
  
  if(status != XN_STATUS_OK)
  {
    std::cout << "ABANDON SHIP" << std::endl;
    std::cout << "Context init failed" << std::endl;
    std::cout << xnGetStatusString(status) << std::endl;
    return 1;
  }
  
  XnStatus statusDepth = ctx.FindExistingNode(XN_NODE_TYPE_DEPTH, depthGen);
  
  if(statusDepth != XN_STATUS_OK)
  {
    std::cout << "couldn't find depth node!" << std::endl;
    std::cout << xnGetStatusString(statusDepth) << std::endl;
    return 1;
  }
  
  XnStatus imageStatus = ctx.FindExistingNode(XN_NODE_TYPE_IMAGE, imgGen);
  
  if(imageStatus != XN_STATUS_OK)
  {
    std::cout << "couldn't find image node!" << std::endl;
    std::cout << xnGetStatusString(imageStatus) << std::endl;
    return 1;
  }
  
  depthGen.GetMetaData(depthMeta);
  imgGen.GetMetaData(imageMeta);
  
  ctx.WaitAnyUpdateAll();
  
  const XnDepthPixel* depthData = depthMeta.Data();
  const XnUInt8* imageData = imageMeta.Data();
  
  XnPixelFormat pf = depthMeta.PixelFormat();
  
  switch(pf)
  {
    case XN_PIXEL_FORMAT_GRAYSCALE_8_BIT:
      std::cout << "grayscale 8bit" << std::endl;
      break;
    case XN_PIXEL_FORMAT_GRAYSCALE_16_BIT:
      std::cout << "grayscale 16bit" << std::endl;
      break;
    case XN_PIXEL_FORMAT_RGB24:
      std::cout << "rgb24" << std::endl;
      break;
    case XN_PIXEL_FORMAT_MJPEG:
      std::cout << "mjpeg" << std::endl;
      break;
    case XN_PIXEL_FORMAT_YUV422:
      std::cout << "yuv422" << std::endl;
      break;
    
  }
  
  zmq::context_t zmq_context(4);
  zmq::socket_t sock(zmq_context, ZMQ_REP);
  
  if(argc == 2)
  {
    char address[30];
    sock.bind(strncat(strncat(strncat(address,"tcp://", 6), argv[1], 15), ":4949",5));
    std::cout << "listening on " << address << std::endl;
  }
  else
  {
    sock.bind("tcp://*:4949");
    std::cout << "listening on all interfaces" << std::endl;
  }
  
  
  
  char *comp_dest = new char[640*480*3]; // worst case, tomorithetetlen
  int reply_size = 0;
  zmq::message_t req;
  zmq::message_t reply;
  
  unsigned char* request_source;
  
  // [0] = compression type
  // [1] = image type
  unsigned char* req_msg;
  xn::MapMetaData *req_meta;
  
  while (true)
  {
    ctx.WaitAnyUpdateAll();
    sock.recv(&req);
    uint64_t start = ClockGetTime();
    std::cout << "received message" << std::endl;
    ctx.WaitNoneUpdateAll();
    
    req_msg = (unsigned char*)req.data();
    if(req.size() < 2)
    {
      std::cout << "bad request, ignored" << std::endl;
      continue;
    }
    
    switch(req_msg[1])
    {
      case IMG_TYPE_DEPTH:
        request_source = (unsigned char*)depthData;
        req_meta = &depthMeta;
        break;
      case IMG_TYPE_IMAGE:
        request_source = (unsigned char*)imageData;
        req_meta = &imageMeta;
        break;
      case IMG_TYPE_IR:
        std::cout << "sorry, IR is unsupported, dropping request" << std::endl;
        break;
      default:
        std::cout << "unknown image type requested, dropping request" << std::endl;
        break;
    }
    
    switch(req_msg[0])
    {
      case COMPR_TYPE_LZ4:
        reply_size = LZ4_compress((const char*)request_source, 
                                 comp_dest, 
                                 640*480* req_meta->BytesPerPixel());
        break;
      case COMPR_TYPE_NONE:
        reply_size = 640*480* req_meta->BytesPerPixel();
        break;
      default:
        std::cout << "unknown compression type requested? code: " << req_msg[0] << std::endl;
        break;
    }
    
    reply.rebuild(reply_size);
    
    if (req_msg[0] != COMPR_TYPE_NONE)
      memcpy((void*)reply.data(), comp_dest, reply_size); // compressed
    else
      memcpy((void*)reply.data(), request_source, reply_size); // uncompressed
    
    
    sock.send(reply);
  }
  
  std::cout << "derp" << std::endl;
} 