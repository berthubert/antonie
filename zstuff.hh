#pragma once
#include <string>
#include <zlib.h>
#include <stdio.h>
#include <map>
#include <stdexcept>
#include <boost/utility.hpp>
#include <memory>

class LineReader
{
public:
  virtual ~LineReader() {}
  // virtual bool getLine(std::string* str) = 0;
  virtual char* fgets(char* line, int num) = 0;
  virtual void seek(uint64_t pos) = 0;
  virtual uint64_t getUncPos()=0;
  static std::unique_ptr<LineReader> make(const std::string& fname);
};



class PlainLineReader : public LineReader, boost::noncopyable
{
public:
  PlainLineReader(const std::string& fname);
  ~PlainLineReader();
  //  bool getLine(std::string* str) = 0;
  char* fgets(char* line, int num);
  void seek(uint64_t pos);
  uint64_t getUncPos();
private:
  FILE* d_fp;
};


class ZLineReader : public LineReader, boost::noncopyable
{
public:
  ZLineReader(const std::string& fname);
  ~ZLineReader();
  //  bool getLine(std::string* str);
  char* fgets(char* line, int num);
  
  uint64_t getUncPos()
  {
    return d_uncPos;
  }
  void seek(uint64_t pos);
private:
  bool getChar(char* c);
  void skip(uint64_t toSkip);
  FILE* d_fp;
  
  struct ZState {
    ZState();
    ZState(const ZState& orig);
    ~ZState();
    ZState& operator=(const ZState& rhs);
    uint64_t fpos;
    z_stream s;
  } d_zs;
  int d_have;
  int d_datapos;

  char d_inbuffer[4096], d_outbuffer[32768];
  std::map<uint64_t, ZState> d_restarts;
  uint64_t d_uncPos;
  bool d_haveSeeked;
};
