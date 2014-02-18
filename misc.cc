#include "misc.hh"
#include <string.h>
#include <stdexcept>
#include <algorithm>
using namespace std;
#include <sys/types.h>
#include <sys/stat.h>

//! read a line of text from a FILE* to a std::string, returns false on 'no data'
bool stringfgets(FILE* fp, std::string* line)
{
  char buffer[1024];   
  line->clear();
  
  do {
    if(!fgets(buffer, sizeof(buffer), fp))
      return !line->empty();
    
    line->append(buffer);
  } while(!strchr(buffer, '\n'));
  return true;
}


uint64_t filesize(const char* name)
{
  struct stat buf;
  if(!stat(name, &buf)) {
    return buf.st_size;
  }
  return 0;
}



char* sfgets(char* p, int num, FILE* fp)
{
  char *ret = fgets(p, num, fp);
  if(!ret) 
    throw std::runtime_error("Unexpected EOF");
  return ret;
}

void chomp(char* line)
{
  char *p;
  p = strchr(line, '\r');
  if(p)*p=0;
  p = strchr(line, '\n');
  if(p)*p=0;
}

void reverseNucleotides(std::string* nucleotides)
{
  std::reverse(nucleotides->begin(), nucleotides->end());
  for(string::iterator iter = nucleotides->begin(); iter != nucleotides->end(); ++iter) {
    if(*iter == 'C')
      *iter = 'G';
    else if(*iter == 'G')
      *iter = 'C';
    else if(*iter == 'A')
      *iter = 'T';
    else if(*iter == 'T')
      *iter = 'A';
  }
}

string compilerVersion()
{
#if defined(__clang__)
  return string("clang " __clang_version__);
#elif defined(__GNUC__)
  return string("gcc " __VERSION__);
#else  // add other compilers here 
  return string("Unknown compiler");
#endif
}
