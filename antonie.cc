/* (C) 2013 TU Delft
   (C) 2013 Netherlabs Computer Consulting BV */

#define __STDC_FORMAT_MACROS
#include <tclap/CmdLine.h>
#include <stdio.h>
#include <iostream>
#include <stdint.h>
#include <stdlib.h>
#include <map>
#include <unordered_map>
#include <vector>
#include <string>
#include <string.h>
#include <stdexcept>
#include <forward_list>
#include <inttypes.h>
#include <algorithm>
#include <numeric>
#include <unistd.h>
#include <errno.h>
#include <math.h>
#include <boost/progress.hpp>
#include <boost/format.hpp>
#include <boost/iostreams/tee.hpp>
#include <boost/iostreams/stream.hpp>
#include <boost/algorithm/string.hpp>
#include <boost/lexical_cast.hpp>
#include "dnamisc.hh"
#include <fenv.h>
#include <memory>
#include <sstream>
#include "geneannotated.hh"
#include "misc.hh"
#include "fastq.hh"

#include <mba/diff.h>
#include <mba/msgno.h>
#include "antonie.hh"
#include "saminfra.hh"

using namespace std;

using namespace boost::algorithm;

extern "C" {
#include "hash.h"
}

namespace io = boost::iostreams;
typedef io::tee_device<std::ostream, std::ostringstream> TeeDevice;
typedef io::stream< TeeDevice > TeeStream;
TeeStream* g_log;

struct FASTQMapping
{
  uint64_t pos;
  bool reverse;
  int indel; // 0 = nothing, >0 means WE have an insert versus reference at pos
             // <0 means WE have a delete versus reference at pos
};

struct GenomeLocusMapping
{
  GenomeLocusMapping() : coverage(0) {}
  forward_list<FASTQMapping> d_fastqs;
  unsigned int coverage;
};

struct Unmatched
{
  string left, unmatched, right;
  dnapos_t pos;
};

class DuplicateCounter
{
public:
  DuplicateCounter(int estimate=1000000)
  {
    d_hashes.reserve(estimate);
  }
  void feedString(const std::string& str);
  void clear();
  typedef map<uint64_t,uint64_t> counts_t;

  counts_t getCounts();
private:
  vector<uint32_t> d_hashes;
};

void DuplicateCounter::feedString(const std::string& str)
{
  uint32_t hashval = hash(str.c_str(), str.length(), 0);
  d_hashes.push_back(hashval);
}

DuplicateCounter::counts_t DuplicateCounter::getCounts()
{
  counts_t ret;
  sort(d_hashes.begin(), d_hashes.end());
  uint64_t repeatCount=1;
  for(auto iter = next(d_hashes.begin()) ; iter != d_hashes.end(); ++iter) {
    if(*prev(iter) != *iter) {
      ++ret[min(repeatCount, (decltype(repeatCount))20)];
      repeatCount=1;
    }
    else
      repeatCount++;
  }
  ++ret[repeatCount]; 
  return ret;
}

void DuplicateCounter::clear()
{
  d_hashes.clear();
  d_hashes.shrink_to_fit();
}

class ReferenceGenome
{
public:
  ReferenceGenome(const string& fname);
  dnapos_t size() const {
    return d_genome.size() - 1; // we pad at the beginning so we are 1 based..
  }
  vector<uint32_t> getMatchingHashes(const vector<uint32_t>& hashes);
  vector<dnapos_t> getReadPositions(const std::string& nucleotides)
  {
    vector<dnapos_t> ret;
    if(nucleotides.length() != d_indexlength)
      throw runtime_error("Attempting to find a read of length we've not indexed for ("+boost::lexical_cast<string>(nucleotides.length())+")");
      
    uint32_t hashval = hash(nucleotides.c_str(), nucleotides.length(), 0);
    HashPos hp(hashval, 0);
    pair<index_t::const_iterator, index_t::const_iterator> range = equal_range(d_index.begin(), d_index.end(), hp);
    if(range.first == range.second)
      return ret;

    for(;range.first != range.second; range.first++) {
      if(!memcmp(d_genome.c_str() + range.first->d_pos, nucleotides.c_str(), nucleotides.length())) {
        ret.push_back(range.first->d_pos);
      }
    }
    return ret;
  }
  
  dnapos_t getReadPosBoth(FastQRead* fq, int qlimit) // tries original & complement
  {
    vector<dnapos_t> positions;
    string nucleotides;
    for(int tries = 0; tries < 2; ++tries) {
      positions = getReadPositions(fq->d_nucleotides);

      if(!positions.empty()) {
        int rpos=random() % positions.size();
        cover(positions[rpos], fq->d_nucleotides.size(), fq->d_quality, qlimit);

        return positions[rpos];
      }
      fq->reverse();
    }
    return dnanpos;
  }

  void cover(dnapos_t pos, unsigned int length, const std::string& quality, int limit) 
  {
    const char* p = quality.c_str();
    for(unsigned int i = 0; i < length; ++i) {
      if(p[i] > limit)
        d_mapping[pos+i].coverage++;
    }
  }

  void cover(dnapos_t pos, char quality, int limit) 
  {
    if(quality > (int) limit)
      d_mapping[pos].coverage++;
  }

  void mapFastQ(dnapos_t pos, const FastQRead& fqfrag, int indel=0)
  {
    FASTQMapping fqm;
    fqm.pos=fqfrag.position;
    fqm.reverse = fqfrag.reversed;
    fqm.indel = indel;
    d_mapping[pos].d_fastqs.push_front(fqm);
    //    cout<<"Adding mapping at pos "<<pos<<", indel = "<<indel<<", reverse= "<<fqm.reverse<<endl;
  }

  vector<dnapos_t> getGCHisto();

  string snippet(dnapos_t start, dnapos_t stop) const { 
    if(stop > d_genome.size()) {
      return d_genome.substr(start);
    }
    return d_genome.substr(start, stop-start);
  }
  void printCoverage(FILE* jsfp, const std::string& fname);
  void index(unsigned int length);

  string getMatchingFastQs(dnapos_t pos, FASTQReader& fastq); 
  string getMatchingFastQs(dnapos_t start, dnapos_t stop,  FASTQReader& fastq); 
  vector<GenomeLocusMapping> d_mapping;
  vector<unsigned int> d_correctMappings, d_wrongMappings, d_gcMappings, d_taMappings;
  vector<vector<uint32_t>> d_kmerMappings;
  vector<Unmatched> d_unmRegions;
  struct LociStats
  {
    vector<std::tuple<char,char,char>> samples;
  };
  dnapos_t d_aCount, d_cCount, d_gCount, d_tCount;
  typedef unordered_map<dnapos_t, LociStats> locimap_t;
  locimap_t d_locimap;
  unordered_map<dnapos_t, unsigned int> d_insertCounts;
  string d_name;
  unsigned int d_indexlength;
private:
  string d_genome;
  struct HashPos {
    HashPos(uint32_t hash_, dnapos_t pos) : d_hash(hash_), d_pos(pos)
    {}
    HashPos(){}
    uint32_t d_hash;
    dnapos_t d_pos;
    
    bool operator<(const HashPos& rhs) const 
    {
      return d_hash < rhs.d_hash;
    }
  };

  typedef vector<HashPos> index_t;
  index_t d_index;
};

ReferenceGenome::ReferenceGenome(const string& fname)
{
  FILE* fp = fopen(fname.c_str(), "r");
  if(!fp)
    throw runtime_error("Unable to open reference genome file '"+fname+"'");
  d_genome.reserve(filesize(fname.c_str()));  // slight overestimate which is great

  char line[256]="";

  sfgets(line, sizeof(line), fp);
  chomp(line);

  if(line[0] != '>') 
    throw runtime_error("Input not FASTA");
  (*g_log)<<"Reading FASTA reference genome of '"<<line+1<<"'\n";
  char* spacepos=strchr(line+1, ' ');
  if(spacepos)
    *spacepos=0;
  d_name=line+1;

  d_genome="*"; // this gets all our offsets ""right""
  while(fgets(line, sizeof(line), fp)) {
    chomp(line);
    d_genome.append(line);
  }
  
  d_aCount = d_cCount = d_gCount = d_tCount = 0;
  for(auto c : d_genome) {
    if(c=='A') ++d_aCount;
    else if(c=='C') ++d_cCount;
    else if(c=='G') ++d_gCount;
    else if(c=='T') ++d_tCount;
  }

  d_mapping.resize(d_genome.size());
  d_indexlength=0;
}

// returns as if we sampled once per index length, an array of index length bins
vector<dnapos_t> ReferenceGenome::getGCHisto()
{
  vector<dnapos_t> ret;
  ret.resize(d_indexlength+1);
  for(dnapos_t pos = 0; pos < d_genome.size() ; pos += d_indexlength/4) {
    ret[round(d_indexlength*getGCContent(snippet(pos, pos + d_indexlength)))]++;
  }
  for(auto& c : ret) {
    c/=4;
  }
  return ret;
}

void ReferenceGenome::index(unsigned int length)
{
  if(length > d_correctMappings.size()) {
    d_correctMappings.resize(length);
    d_wrongMappings.resize(length);
    d_taMappings.resize(length);
    d_gcMappings.resize(length);
    d_kmerMappings.resize(length);
  }

  d_index.clear();
  d_index.reserve(d_genome.length());
  d_indexlength=length;
  (*g_log)<<"Indexing "<<d_genome.length()<<" nucleotides for length "<<length<<"..";
  for(string::size_type pos = 0 ; pos < d_genome.length() - length; ++pos) {
    uint32_t hashval = hash(d_genome.c_str() + pos, d_indexlength, 0);
    d_index.push_back(HashPos(hashval, pos));
  }

  (*g_log)<<" done\nSorting hashes..";
  sort(d_index.begin(), d_index.end());
  (*g_log)<<" done"<<endl;
  uint64_t diff = 0;

  for(auto iter = d_index.begin(); iter!= d_index.end() ; ++iter) {
    if(iter != d_index.begin() && iter->d_hash != prev(iter)->d_hash) {
      diff++;
    }
  }
  (*g_log)<<"Average hash fill: "<<1.0*d_genome.length()/diff<<endl;
}

string ReferenceGenome::getMatchingFastQs(dnapos_t pos, FASTQReader& fastq)
{
  return getMatchingFastQs(pos > 150 ? pos-150 : 0, pos+150, fastq);
}

string ReferenceGenome::getMatchingFastQs(dnapos_t start, dnapos_t stop, FASTQReader& fastq) 
{
  ostringstream os;

  string reference=snippet(start, stop);
  unsigned int insertPos=0;
  for(unsigned int i = 0 ; i < stop - start; ++i) {
    if(i== (stop-start)/2)
      os << reference << endl;
    string spacer(i, ' ');
    for(auto& fqm : d_mapping[start+i].d_fastqs) {
      fastq.seek(fqm.pos);
      FastQRead fqr;
      fastq.getRead(&fqr);
      if(fqm.reverse)
        fqr.reverse();

      if(fqm.indel > 0 && !insertPos) { // our read has an insert at this position, stretch reference
        if(i+fqm.indel < reference.size())
          reference.insert(i+fqm.indel, 1, '_');
        insertPos=i+fqm.indel;
      } else if(fqm.indel < 0) {      // our read has an erase at this position
        fqr.d_nucleotides.insert(-fqm.indel, 1, 'X');
        fqr.d_quality.insert(-fqm.indel, 1, 42);
      }
      
      if(fqm.indel <= 0 && insertPos && i > insertPos) {
        fqr.d_nucleotides.insert(0, 1, '<');
        fqr.d_quality.insert(0, 1, 40);
      }
      os << spacer;
      int offset=0;
      for(unsigned int j = 0 ; j < fqr.d_nucleotides.size() && i + j + offset < reference.size(); ++j) {
        if(reference[i+j]=='_' && !fqm.indel) {
          os<<'_';
          offset=1;
        }
        if(reference[i+j+offset]==fqr.d_nucleotides[j])
          os<<'.';
        else if(fqr.d_quality[j] > 30) 
          os << fqr.d_nucleotides[j];
        else if(fqr.d_quality[j] < 22) 
          os << ' ';
        else
          os<< (char)tolower(fqr.d_nucleotides[j]);
      }
      os<<"                 "<<(fqm.reverse ? 'R' : ' ');
      os<<endl;
    }
  }
  return os.str();
}

template<typename T>
string jsonVector(const vector<T>& v, const std::string& name, 
		  std::function<double(dnapos_t)> yAdjust = [](dnapos_t d){return 1.0*d;},
		  std::function<double(int)> xAdjust = [](int i){return 1.0*i;})
{
  ostringstream ret;
  ret << "var "<<name<<"=[";
  for(auto iter = v.begin(); iter != v.end(); ++iter) {
    if(iter != v.begin())
      ret<<',';
    ret << '[' << xAdjust(iter - v.begin()) <<','<< yAdjust(*iter)<<']';
  }
  ret <<"];\n";
  return ret.str();
}


template<typename T>
string jsonVectorD(const vector<T>& v, const std::string& name, 
		  std::function<double(double)> yAdjust = [](double d){return d;},
		  std::function<double(int)> xAdjust = [](int i){return 1.0*i;})
{
  ostringstream ret;
  ret << "var "<<name<<"=[";
  for(auto iter = v.begin(); iter != v.end(); ++iter) {
    if(iter != v.begin())
      ret<<',';
    ret << '[' << xAdjust(iter - v.begin()) <<','<< yAdjust(*iter)<<']';
  }
  ret <<"];\n";
  return ret.str();
}


vector<uint32_t> ReferenceGenome::getMatchingHashes(const vector<uint32_t>& hashes)
{
  struct cmp
  {
    bool operator()(const uint32_t& a, const HashPos& b) const
    {
      return a < b.d_hash;
    }
    bool operator()(const HashPos& a, const uint32_t& b) const
    {
      return a.d_hash < b;
    }

  };
  vector<uint32_t> intersec;
  set_intersection(hashes.begin(), hashes.end(), d_index.begin(), d_index.end(), back_inserter(intersec), cmp());

  return intersec;
}

void ReferenceGenome::printCoverage(FILE* jsfp, const std::string& histoName)
{
  uint64_t totCoverage=0, noCoverages=0;
  unsigned int cov;

  bool wasNul=true;
  string::size_type prevNulpos=0;

  vector<unsigned int> covhisto;
  covhisto.resize(65535);
  d_unmRegions.clear();
  for(string::size_type pos = 0; pos < d_mapping.size(); ++pos) {
    cov = d_mapping[pos].coverage;
    bool noCov = cov < 2;
    if(cov > covhisto.size()) {
      cerr<<"anomalous coverage: "<<cov<<endl;
    }
    else {
      covhisto[cov]++;
    }
    totCoverage += cov;

    if(noCov) {
      noCoverages++;
    }
    
    if(!noCov && wasNul) {
      if(prevNulpos > 40 && pos + 40 < d_genome.length()) {
	Unmatched unm;
	unm.left = d_genome.substr(prevNulpos-40, 40);
	unm.right = d_genome.substr(pos, 40);
	unm.unmatched = d_genome.substr(prevNulpos, pos-prevNulpos);
	unm.pos = prevNulpos;
	d_unmRegions.push_back(unm);
      }
      wasNul=false;
    }
    else if(noCov && !wasNul) {
      wasNul=true;
      prevNulpos = pos;
    }
  }

  Clusterer<Unmatched> cl(100);
  
  for(auto unm : d_unmRegions) {
    cl.feed(unm);
  }

  (*g_log) << (boost::format("Average depth: %|40t|    %10.2f\n") % (1.0*totCoverage/d_mapping.size())).str();
  (*g_log) << (boost::format("Undercovered nucleotides: %|40t| %10d (%.2f%%), %d ranges\n") % noCoverages % (noCoverages*100.0/d_mapping.size()) % cl.d_clusters.size()).str();

  // snip off the all-zero part at the end
  for(auto iter = covhisto.rbegin(); iter != covhisto.rend(); ++iter) {
    if(*iter!=0) {
      covhisto.resize(covhisto.size() - (iter - covhisto.rbegin()));
      break;
    }
  }
  uint64_t total = std::accumulate(covhisto.begin(), covhisto.end(), 0);
  fputs(jsonVector(covhisto, histoName, [&total](dnapos_t dp) { return 1.0*dp/total; }).c_str(), jsfp);
}

uint32_t kmerMapper(const std::string& str, int offset, int unsigned len)
{
  uint32_t ret=0;
  const char *c=str.c_str() + offset;
  string::size_type val;
  for(string::size_type i = 0; i != len; ++i, ++c) {
    ret<<=2;
    if(*c=='A') val=0;
    else if(*c=='C') val=1;
    else if(*c=='G') val=2;
    else if(*c=='T') val=3;
    else 
      continue;

    ret |= val;
  }
  return ret;
}

// 0 if nothing interesting, positive if our read has insert at that position, negative if we have a delete at that position
int MBADiff(dnapos_t pos, const FastQRead& fqr, const string& reference)
{
  string::size_type n, m, d;
  int sn;
  struct varray *ses = varray_new(sizeof(struct diff_edit), NULL);
  
  n = reference.length();
  m = fqr.d_nucleotides.length();
  if ((d = diff(reference.c_str(), 0, n, fqr.d_nucleotides.c_str(), 0, m, NULL, NULL, NULL, 0, ses, &sn, NULL)) == -1) {
    MMNO(errno);
    printf("Error\n");
    return EXIT_FAILURE;
  }
  int ret=0;

#if 0
  if(pos > 3422100 && pos < 3422300) {
    printf("pos %u, d=%lu sn=%d\nUS:  %s\nREF: %s\n", pos, d, 
	   sn, fqr.d_nucleotides.c_str(), reference.c_str());

    for (int i = 0; i < sn; i++) {
      struct diff_edit *e = (struct diff_edit*)varray_get(ses, i);
      
      switch (e->op) {
      case DIFF_MATCH:
	printf("MAT: ");
	fwrite(fqr.d_nucleotides.c_str() + e->off, 1, e->len, stdout);
	break;
      case DIFF_INSERT:
	printf("INS: ");
	fwrite(reference.c_str() + e->off, 1, e->len, stdout);
	break;
      case DIFF_DELETE:
	printf("DEL: ");
	fwrite(fqr.d_nucleotides.c_str() + e->off, 1, e->len, stdout);
	break;
      }
      printf("\n");
    }
  }
#endif 
  if(sn == 4 && d == 2) {
    struct diff_edit *match1 = (struct diff_edit*)varray_get(ses, 0),
      *change1=(struct diff_edit*)varray_get(ses, 1), 
      *match2=(struct diff_edit*)varray_get(ses, 2), 
      *change2=(struct diff_edit*)varray_get(ses, 3);
    
    if(match1->op == DIFF_MATCH && match2->op==DIFF_MATCH) {
      if(change1->op == DIFF_DELETE && change2->op==DIFF_INSERT) {
	//	cout << "Have delete of "<<change1->len<<" in our read at " << pos+change1->off <<endl;
        ret=-change1->off;
      }
      else if(change1->op == DIFF_INSERT && change2->op==DIFF_DELETE) {
	//        cout<<"Have insert of "<<change1->len<<" in our read at "<<pos+change1->off<<endl;
        ret=change1->off;
      }
    }
  }

  varray_del(ses);

  if(sn > 6)
    return 0;

  return ret;
}

unsigned int diffScore(ReferenceGenome& rg, dnapos_t pos, FastQRead& fqfrag, int qlimit)
{
  unsigned int diffcount=0;
  string reference = rg.snippet(pos, pos + fqfrag.d_nucleotides.length());
  for(string::size_type i = 0; i < fqfrag.d_nucleotides.size() && i < reference.size();++i) {
    if(fqfrag.d_nucleotides[i] != reference[i] && fqfrag.d_quality[i] > qlimit) 
      diffcount++;
  }

  if(diffcount >= 5) { // bit too different, try mbadiff!
    int res=MBADiff(pos, fqfrag, reference);
    if(res < 0 || res > 0)
      return 1;
  }

  return diffcount;
}

vector<dnapos_t> getTriplets(const vector<pair<dnapos_t, char>>& together, unsigned int interval, unsigned int shift) 
{
  vector<dnapos_t> ret;

  for(unsigned int i = 0; i < together.size() - 2; ++i) {
    if(together[i].second=='L' && together[i+1].second=='M' && together[i+2].second=='R' && 
       together[i+1].first - together[i].first < 1.2*interval && together[i+2].first - together[i+1].first < 1.2*interval) {
      dnapos_t lpos;
      lpos=together[i].first; 

      if(lpos < shift)
        continue;
      
      ret.push_back(lpos-shift);
    }
  }         
  return ret;
}

void printCorrectMappings(FILE* jsfp, const ReferenceGenome& rg, const std::string& name)
{
  fprintf(jsfp, "var %s=[", name.c_str());
  for(unsigned int i=0; i < rg.d_correctMappings.size() ;++i) {
    if(!rg.d_correctMappings[i] || !rg.d_wrongMappings[i])
      continue;
    double total=rg.d_correctMappings[i] + rg.d_wrongMappings[i];
    double error= rg.d_wrongMappings[i]/total;
    double qscore=-10*log10(error);
    fprintf(jsfp, "%s[%d,%.2f]", i ? "," : "", i, qscore);
    //    cout<<"total "<<total<<", error: "<<error<<", qscore: "<<qscore<<endl;
  }
  fprintf(jsfp,"];\n");
}

void printGCMappings(FILE* jsfp, const ReferenceGenome& rg, const std::string& name)
{
  fprintf(jsfp, "var %s=[", name.c_str());
  for(unsigned int i=0; i < rg.d_correctMappings.size() ;++i) {
    double total=rg.d_gcMappings[i] + rg.d_taMappings[i];
    double ratio= rg.d_gcMappings[i]/total;

    fprintf(jsfp, "%s[%d,%.2f]", i ? "," : "", i, ratio);
  }
  fprintf(jsfp,"];\n");
}

struct qtally
{
  qtally() : correct{0}, incorrect{0}{}
  uint64_t correct;
  uint64_t incorrect;
};

string DNADiff(ReferenceGenome& rg, dnapos_t pos, FastQRead& fqfrag, int qlimit, SAMWriter* sw, vector<qtally>* qqcounts)
{
  string reference = rg.snippet(pos, pos + fqfrag.d_nucleotides.length());

  double diffcount=0;
  for(string::size_type i = 0; i < fqfrag.d_nucleotides.size() && i < reference.size();++i) {
    if(fqfrag.d_nucleotides[i] != reference[i]) {
      if(fqfrag.d_quality[i] > qlimit) 
	diffcount++;
      else
	diffcount+=0.5;
    }
  }

  if(diffcount < 5) {
    rg.mapFastQ(pos, fqfrag);
    if(sw)
      sw->write(pos, fqfrag);
  }
  else {
    int indel=MBADiff(pos, fqfrag, reference);
    if(indel) {
      rg.mapFastQ(pos, fqfrag, indel);
      if(sw)
	sw->write(pos, fqfrag, indel); 
      diffcount=1;
      if(indel > 0) { // our read has an insert at this position
        fqfrag.d_nucleotides.erase(indel, 1); // this makes things align again
        fqfrag.d_quality.erase(indel, 1); 
        rg.d_insertCounts[pos+indel]++;
      } else {      // our read has an erase at this position
        fqfrag.d_nucleotides.insert(-indel, 1, 'X');
        fqfrag.d_quality.insert(-indel, 1, 40);
      }
    }
  }

  string diff;
  diff.reserve(fqfrag.d_nucleotides.length());

  unsigned int readMapPos;
  for(string::size_type i = 0; i < fqfrag.d_nucleotides.size() && i < reference.size();++i) {
    readMapPos = fqfrag.reversed ? ((fqfrag.d_nucleotides.length()- 1) - i) : i;
    char c =  fqfrag.d_nucleotides[i];

    if(c != reference[i]) {
      diff.append(1, fqfrag.d_quality[i] > qlimit ? '!' : '^');
      if(fqfrag.d_quality[i] > qlimit && diffcount < 5) 
        rg.d_locimap[pos+i].samples.push_back(std::make_tuple(fqfrag.d_nucleotides[i], fqfrag.d_quality[i], 
								fqfrag.reversed ^ (i > fqfrag.d_nucleotides.length()/2))); // head or tail
      
      if(diffcount < 5) {
	unsigned int q = (unsigned int)fqfrag.d_quality[i];
	(*qqcounts)[q].incorrect++;
	rg.d_wrongMappings[readMapPos]++;
      }
    }
    else {
      diff.append(1, ' ');
      rg.cover(pos+i,fqfrag.d_quality[i], qlimit);
      //      if(diffcount < 5) {
	(*qqcounts)[(unsigned int)fqfrag.d_quality[i]].correct++;
	rg.d_correctMappings[readMapPos]++;
	//}
    }
  }
  //  if(diffcount > 5) {
  //  cout<<"US:  "<<fqfrag.d_nucleotides<<endl<<"DIF: ";
  //  cout<<diff<<endl<<"REF: "<<reference<<endl;
  //  cout<<"QUA: "<<fqfrag.d_quality<<endl;
  //  cout<<endl;
  //}
  return diff;
}


void emitRegion(FILE*fp, ReferenceGenome& rg, FASTQReader& fastq, GeneAnnotationReader& gar, const string& name, unsigned int index, dnapos_t start, dnapos_t stop)
{
  dnapos_t dnapos = (start+stop)/2;
  fprintf(fp, "region[%d]={name:'%s', pos: %d, depth: [", index, name.c_str(), dnapos);
  for(dnapos_t pos = start; pos < stop; ++pos) {
    if(pos != start) 
      fprintf(fp, ",");
    fprintf(fp, "[%d,%d]", pos, rg.d_mapping[pos].coverage);
  }
  string picture=rg.getMatchingFastQs(start, stop, fastq);
  replace_all(picture, "\n", "\\n");
  
  string annotations;
  auto gas=gar.lookup(dnapos);
  for(auto& ga : gas) {
    annotations += ga.name+" [" + ga.tag  + "], ";
  }
  
  fprintf(fp,"], picture: '%s', annotations: '%s'};\n", picture.c_str(), annotations.c_str());
  
  fputs("\n", fp);
  fflush(fp);
}

void emitRegion(FILE*fp, ReferenceGenome& rg, FASTQReader& fastq, GeneAnnotationReader& gar, const string& name, unsigned int index, dnapos_t start)
{
  emitRegion(fp, rg, fastq, gar, name, index, start-200, start +200);
}


unsigned int variabilityCount(const ReferenceGenome& rg, dnapos_t position, const ReferenceGenome::LociStats& lc, double* fraction)
{
  vector<int> counts(256);
  counts[rg.snippet(position, position+1)[0]]+=rg.d_mapping[position].coverage;
  
  int forwardCount=0;

  for(auto& j : lc.samples) {
    counts[get<0>(j)]++;
    if(get<2>(j))
      forwardCount++;
  }
  sort(counts.begin(), counts.end());
  unsigned int nonDom=0;
  for(unsigned int i=0; i < 255; ++i) {
    nonDom+=counts[i];
  }
  
  if(nonDom + counts[255] < 20)
    return 0;

  *fraction = 1.0*forwardCount / (1.0*lc.samples.size());
  if(*fraction < 0.05 || *fraction > 0.95)
    return 0;

  return 100*nonDom/counts[255];
}


dnapos_t halfFind(const std::vector<int64_t>& fqpositions, FASTQReader &fastq, ReferenceGenome& rg, int keylen)
{
  boost::progress_display fuzzyProgress(fqpositions.size(), cerr);
  vector<dnapos_t> stillUnfound;
  FastQRead fqfrag;
  for(auto fqpos : fqpositions) {
    ++fuzzyProgress;
    fastq.seek(fqpos);
    fastq.getRead(&fqfrag);

    if(!rg.getReadPositions(fqfrag.d_nucleotides.substr(0, keylen)).empty())
      continue;
    if(!rg.getReadPositions(fqfrag.d_nucleotides.substr(keylen, keylen)).empty())
      continue;

    fqfrag.reverse();
    if(!rg.getReadPositions(fqfrag.d_nucleotides.substr(0, keylen)).empty())
      continue;
    if(!rg.getReadPositions(fqfrag.d_nucleotides.substr(keylen, keylen)).empty())
      continue;
    
    stillUnfound.push_back(fqpos);
  }
  return fqpositions.size()  - stillUnfound.size();
}

int fuzzyFind(std::vector<uint64_t>* fqpositions, FASTQReader &fastq, ReferenceGenome& rg, SAMWriter* sw, vector<qtally>* qqcounts, int keylen, int qlimit)
{
  boost::progress_display fuzzyProgress(fqpositions->size(), cerr);
  vector<uint64_t> stillUnfound;
  FastQRead fqfrag;
  string left, middle, right;
  typedef pair<dnapos_t, char> tpos;
  vector<dnapos_t> lpositions, mpositions, rpositions;
  unsigned int fuzzyFound=0;
  for(auto fqpos : *fqpositions) {
    fastq.seek(fqpos);
    fastq.getRead(&fqfrag);
    
    struct Match 
    {
      Match() = default;
      Match(unsigned int score_, bool reversed_) : score(score_), reversed(reversed_){}
      unsigned int score;
      bool reversed;
    };
    map<dnapos_t, Match> allMatches;

    unsigned int interval=(fqfrag.d_nucleotides.length() - 3*keylen)/3;

    for(unsigned int attempts=0; attempts < interval; attempts += 3) {
      for(int tries = 0; tries < 2; ++tries) {
        if(tries)
          fqfrag.reverse();
        left=fqfrag.d_nucleotides.substr(attempts, keylen);   middle=fqfrag.d_nucleotides.substr(interval+attempts, keylen); right=fqfrag.d_nucleotides.substr(2*interval+attempts, keylen);
        lpositions=rg.getReadPositions(left); 
        if(lpositions.empty())
          continue;
        mpositions=rg.getReadPositions(middle); 
        if(mpositions.empty())
          continue;
        rpositions=rg.getReadPositions(right);

        if(lpositions.size() + mpositions.size() + rpositions.size() < 3)
          continue;
        vector<tpos> together;
        for(auto fpos: lpositions) { together.push_back(make_pair(fpos, 'L')); }        
        for(auto fpos: mpositions) { together.push_back(make_pair(fpos, 'M')); }
        for(auto fpos: rpositions) { together.push_back(make_pair(fpos, 'R')); }
        
        sort(together.begin(), together.end());

        auto matches=getTriplets(together, interval, attempts);
        int score;
        for(auto match : matches) {
          if(allMatches.count(match))
            continue;
             
          score = diffScore(rg, match, fqfrag, qlimit);
          
          allMatches[match] = Match{score, fqfrag.reversed};
          if(score==0) // won't get any better than this
            goto done;
        }
      }
    }
  done:;
    if(!allMatches.empty()) {
      //      cout<<"Have "<<allMatches.size()<<" different matches"<<endl;
      if(allMatches.size()==1) {
        // cout<<"  Position "<<allMatches.begin()->first<<" had score "<<allMatches.begin()->second.score<<endl;
        if(fqfrag.reversed != allMatches.begin()->second.reversed)
          fqfrag.reverse();

	DNADiff(rg, allMatches.begin()->first, fqfrag, qlimit, sw, qqcounts);
      } 
      else {
        map<unsigned int, vector<pair<dnapos_t, bool>>> scores;
        for(auto match: allMatches) {
	  //          cout<<"  Position "<<match.first<<" had score "<<match.second.score<<endl;
          scores[match.second.score].push_back(make_pair(match.first, match.second.reversed));
        }
        
        const auto& first = scores.begin()->second;
        auto pick = first[random() % first.size()];
	//        cout<<" Picking: "<< pick.first <<endl;
        if(fqfrag.reversed != pick.second)
          fqfrag.reverse();
        DNADiff(rg, pick.first, fqfrag, qlimit, sw, qqcounts);
      }
      fuzzyFound++;
    }
    else {
      stillUnfound.push_back(fqpos);
    }

    ++fuzzyProgress;
  }
  stillUnfound.swap(*fqpositions);
  cerr<<"\r";
  
  return fuzzyFound;
}

typedef vector<VarMeanEstimator> qstats_t;

void writeUnmatchedReads(const vector<uint64_t>& unfoundReads, FASTQReader& fastq)
{
  FILE *fp=fopen("unfound.fastq", "w");
  FastQRead fqfrag;
  for(const auto& pos :  unfoundReads) {
    fastq.seek(pos);
    fastq.getRead(&fqfrag);
    fprintf(fp, "@%s\n%s\n+\n%s\n", fqfrag.d_header.c_str(), fqfrag.d_nucleotides.c_str(), fqfrag.getSangerQualityString().c_str());
  }
  fclose(fp);
}

void printQualities(FILE* jsfp, const qstats_t& qstats)
{
  int i=0;

  fprintf(jsfp, "qualities=[");
  for(const auto& q : qstats) {
    if(i)
      fputs(",", jsfp);
    fprintf(jsfp, "[%d, %f]", i, -10.0*log10(mean(q)));
    ++i;
  }
  fputs("];\n", jsfp);

  vector<double> qlo, qhi;
  for(const auto& q : qstats) {
    qlo.push_back(-10.0*log10(mean(q)) - sqrt(-10.0*log10(variance(q))));
    qhi.push_back(-10.0*log10(mean(q)) +sqrt(-10.0*log10(variance(q))));
  }

  fputs((jsonVectorD(qlo,"qlo")+jsonVectorD(qhi, "qhi")).c_str(), jsfp);

  fflush(jsfp);
}

double qToErr(unsigned int i) 
{
  static vector<double> answers;
  
  if(answers.empty()) {
    for(int n = 0; n < 60 ; ++n) {
      answers.push_back(pow(10.0, -n/10.0));
    }
  }
  if(i > answers.size()) {
    throw runtime_error("Can't calculate error rate for Q "+boost::lexical_cast<string>(i));
  }

  return answers[i];
}


int main(int argc, char** argv)
{

#ifndef __APPLE__
  feenableexcept(FE_DIVBYZERO | FE_INVALID); 
#endif 
  
  TCLAP::CmdLine cmd("Command description message", ' ', "0.0");

  TCLAP::ValueArg<std::string> annotationsArg("a","annotations","read annotations for reference genome from this file",false, "", "filename", cmd);
  TCLAP::ValueArg<std::string> referenceArg("r","reference","read annotations for reference genome from this file",true,"","string", cmd);
  TCLAP::ValueArg<std::string> fastqArg("f","fastq","read annotations for reference genome from this file",true,"","string", cmd);
  TCLAP::ValueArg<std::string> excludeArg("x","exclude","read annotations for reference genome from this file",false,"","string", cmd);
  TCLAP::ValueArg<std::string> samFileArg("s","sam-file","Write the assembly to the named SAM file",false,"","filename", cmd);
  TCLAP::ValueArg<int> qualityOffsetArg("q","quality-offset","Quality offset in fastq. 33 for Sanger.",false, 33,"offset", cmd);
  TCLAP::ValueArg<int> beginSnipArg("b","begin-snip","Number of nucleotides to snip from begin of reads",false, 0,"nucleotides", cmd);
  TCLAP::ValueArg<int> endSnipArg("e","end-snip","Number of nucleotides to snip from end of reads",false, 0,"nucleotides", cmd);
  TCLAP::ValueArg<int> qlimitArg("l","qlimit","Disregard nucleotide reads with less quality than this in calls",false, 30,"q", cmd);
  TCLAP::ValueArg<int> duplimitArg("d","duplimit","Ignore reads that occur more than d times. 0 for no filter.",false, 0,"times", cmd);
  TCLAP::SwitchArg unmatchedDumpSwitch("u","unmatched-dump","Create a dump of unmatched reads (unfound.fastq)", cmd, false);

  cmd.parse( argc, argv );

  unsigned int qlimit = qlimitArg.getValue();
  unsigned int duplimit = duplimitArg.getValue();
  srandom(time(0));
  ostringstream jsonlog;  
  TeeDevice td(cerr, jsonlog);
  g_log = new TeeStream(td);

  GeneAnnotationReader gar(annotationsArg.getValue());
  (*g_log)<<"Done reading "<<gar.size()<<" annotations"<<endl;
  
  FASTQReader fastq(fastqArg.getValue(), qualityOffsetArg.getValue(), beginSnipArg.getValue(), endSnipArg.getValue());

  (*g_log)<<"Snipping "<<beginSnipArg.getValue()<<" from beginning of reads, "<<endSnipArg.getValue()<<" from end of reads"<<endl;

  unsigned int bytes=0;
  FastQRead fqfrag;
  bytes=fastq.getRead(&fqfrag); // get a read to index based on its size
  
  ReferenceGenome rg(referenceArg.getValue());
  double genomeGCRatio = 1.0*(rg.d_cCount + rg.d_gCount)/(rg.d_cCount + rg.d_gCount + rg.d_aCount + rg.d_tCount);
  (*g_log)<<"GC Content of reference genome: "<<100.0*genomeGCRatio<<"%"<<endl;
  rg.index(fqfrag.d_nucleotides.size());
  
  unique_ptr<FILE, int(*)(FILE*)> jsfp(fopen("data.js","w"), fclose);

  fprintf(jsfp.get(), "var genomeGCRatio=%f;\n", genomeGCRatio);

  unique_ptr<ReferenceGenome> phix;

  if(!excludeArg.getValue().empty()) {
    phix = unique_ptr<ReferenceGenome>{new ReferenceGenome(excludeArg.getValue())};
    
    (*g_log)<<"Loading positive control filter genome(s)"<<endl;
    
    phix->index(fqfrag.d_nucleotides.size());
  }
  g_log->flush();
  dnapos_t pos;

  uint64_t withAny=0, found=0, notFound=0, total=0, qualityExcluded=0, fuzzyFound=0, 
    phixFound=0, differentLength=0, tooFrequent=0;

  SAMWriter sw(samFileArg.getValue(), rg.d_name, rg.size());

  (*g_log)<<"Performing exact matches of reads to reference genome"<<endl;
  boost::progress_display show_progress(filesize(fastqArg.getValue().c_str()), cerr);
 
  for(auto& kmers : rg.d_kmerMappings) 
    kmers.resize(256); // 4^4, corresponds to the '4' below

  qstats_t qstats;
  qstats.resize(fqfrag.d_nucleotides.size());
  VarMeanEstimator qstat;
  vector<unsigned int> qcounts(256);
  vector<uint64_t> unfoundReads;
  vector<qtally> qqcounts(256);
  vector<dnapos_t> gchisto(fqfrag.d_nucleotides.size()+1);

  DuplicateCounter dc;
  uint32_t theHash;
  map<uint32_t, uint32_t> seenAlready;
  do { 
    show_progress += bytes;
    total++;
    for(string::size_type pos = 0 ; pos < fqfrag.d_quality.size(); ++pos) {
      int i = fqfrag.d_quality[pos];
      double err = qToErr(i);
      qstat(err);
      qstats[pos](err);
      qcounts[i]++;
    }
    dc.feedString(fqfrag.d_nucleotides);
    if(duplimit) {
      theHash=hash(fqfrag.d_nucleotides.c_str(), fqfrag.d_nucleotides.size(), 0);
      if(++seenAlready[theHash] > 4) {
	tooFrequent++;
	continue;
      }
    }

    gchisto[round(fqfrag.d_nucleotides.size()*getGCContent(fqfrag.d_nucleotides))]++;
    bool hadN=false;
    for(string::size_type i = 0 ; i < fqfrag.d_nucleotides.size(); ++i) {
      char c = fqfrag.d_nucleotides[i];
      if(c=='G' || c=='C')
	rg.d_gcMappings[i]++;
      else
	rg.d_taMappings[i]++;

      if(fqfrag.d_nucleotides.size() - i > 4)
	rg.d_kmerMappings[i][kmerMapper(fqfrag.d_nucleotides, i, 4)]++;
      if(c=='N')
	hadN=true;
    }

    if(hadN) {
      unfoundReads.push_back(fqfrag.position);
      withAny++;
      continue;
    }
    if(fqfrag.d_nucleotides.length() != rg.d_indexlength) {
      differentLength++;
      unfoundReads.push_back(fqfrag.position);
      continue;
    }
    pos = rg.getReadPosBoth(&fqfrag, qlimit);
    if(pos == dnanpos ) {
      if(phix && phix->getReadPosBoth(&fqfrag, qlimit)!=dnanpos) {
        phixFound++;
      }
      else {
        unfoundReads.push_back(fqfrag.position);
        notFound++;
      }
    }
    else {
      for(auto c : fqfrag.d_quality) {
	qqcounts[c].correct++;
      }
      rg.mapFastQ(pos, fqfrag);					
      sw.write(pos, fqfrag);
      found++;
    }
  } while((bytes=fastq.getRead(&fqfrag)));
  
  uint64_t totNucleotides=total*fqfrag.d_nucleotides.length();
  fprintf(jsfp.get(), "qhisto=[");
  for(int c=0; c < 50; ++c) {
    fprintf(jsfp.get(), "%s[%d,%f]", c ? "," : "", (int)c, 1.0*qcounts[c]/totNucleotides);
  }
  fprintf(jsfp.get(),"];\n");

  fprintf(jsfp.get(), "var dupcounts=[");
  auto duplicates = dc.getCounts();
  for(auto iter = duplicates.begin(); iter != duplicates.end(); ++iter) {
    fprintf(jsfp.get(), "%s[%ld,%f]", (iter!=duplicates.begin()) ? "," : "", iter->first, 1.0*iter->second/total);
  }
  fprintf(jsfp.get(),"];\n");
  dc.clear(); // might save some memory..

  dnapos_t totalhisto= accumulate(gchisto.begin(), gchisto.end(), 0);
  fputs(jsonVector(gchisto, "gcreadhisto",  
		   [totalhisto](dnapos_t c){return 1.0*c/totalhisto;},
		   [&fqfrag](int i) { return 100.0*i/fqfrag.d_nucleotides.size();}   ).c_str(), 
	jsfp.get());

  fputs(jsonVector(rg.getGCHisto(), "gcrefhisto",  
		   [&fqfrag,&rg](dnapos_t c){return 1.0*c/(rg.size()/fqfrag.d_nucleotides.size());},
		   [&fqfrag](int i) { return 100.0*i/fqfrag.d_nucleotides.size();}   ).c_str(), 
	jsfp.get());



  fprintf(jsfp.get(), "var kmerstats=[");
  unsigned int readOffset=0;
  for(const auto& kmer :  rg.d_kmerMappings) {
    if(readOffset >= fqfrag.d_nucleotides.length() - 4)
      break;

    VarMeanEstimator acc;
    for(auto& count : kmer) {
      acc(count);
    }
    fprintf(jsfp.get(), "%s[%d, %f]", readOffset ? "," : "", readOffset, sqrt(variance(acc)) / mean(acc) );
    readOffset++;
  }
  fprintf(jsfp.get(), "];\n");

  printGCMappings(jsfp.get(), rg, "gcRatios");

  (*g_log) << (boost::format("Total reads: %|40t| %10d (%.2f gigabps)") % total % (totNucleotides/1000000000.0)).str() <<endl;
  (*g_log) << (boost::format("Excluded control reads: %|40t|-%10d") % phixFound).str() <<endl;
  (*g_log) << (boost::format("Quality excluded: %|40t|-%10d") % qualityExcluded).str() <<endl;
  (*g_log) << (boost::format("Ignored reads with N: %|40t|-%10d") % withAny).str()<<endl;
  if(duplimit)
    (*g_log) << (boost::format("Too frequent reads: %|40t| %10d (%.02f%%)") % tooFrequent % (100.0*tooFrequent/total)).str() <<endl;
  (*g_log) << (boost::format("Different length reads: %|40t|-%10d") % differentLength).str() <<endl;
  (*g_log) << (boost::format("Full matches: %|40t|-%10d (%.02f%%)\n") % found % (100.0*found/total)).str();
  (*g_log) << (boost::format("Not fully matched: %|40t|=%10d (%.02f%%)\n") % notFound % (notFound*100.0/total)).str();
  (*g_log) << (boost::format("Mean Q: %|40t|    %10.2f +- %.2f\n") % (-10.0*log10(mean(qstat))) 
	       % sqrt(-10.0*log10(variance(qstat)) )).str();

  seenAlready.clear();

  for(auto& i : rg.d_correctMappings) {
    i=found;
  }

  if(phix) {
    for(auto& i : phix->d_correctMappings) {
      i=phixFound;
    }
  }

  rg.printCoverage(jsfp.get(), "fullHisto");
  printQualities(jsfp.get(), qstats);

  int keylen=11;
  rg.index(keylen);
  if(phix)
    phix->index(keylen);

  (*g_log)<<"Performing sliding window partial matches"<<endl;

  fuzzyFound = fuzzyFind(&unfoundReads, fastq, rg, &sw, &qqcounts, keylen, qlimit);
  uint32_t fuzzyPhixFound = 0;
  if(phix) {
    (*g_log)<<"Performing sliding window partial matches for control/exclude"<<endl;
    fuzzyPhixFound=fuzzyFind(&unfoundReads, fastq, *phix, 0, &qqcounts, keylen, qlimit);
  }
  // (*g_log)<<fuzzyPhixFound<<" fuzzy @ phix"<<endl;

  (*g_log)<<(boost::format("Fuzzy found: %|40t|-%10d\n")%fuzzyFound).str();  
  (*g_log)<<(boost::format("Fuzzy found in excluded set: %|40t|-%10d\n")%fuzzyPhixFound).str();  
  (*g_log)<<(boost::format("Unmatchable reads:%|40t|=%10d (%.2f%%)\n") 
         % unfoundReads.size() % (100.0*unfoundReads.size()/total)).str();

  if(unmatchedDumpSwitch.getValue())
    writeUnmatchedReads(unfoundReads, fastq);

  //  (*g_log)<<"After sliding matching: "<<endl;
  rg.printCoverage(jsfp.get(), "fuzzyHisto");
  int index=0;

  Clusterer<Unmatched> cl(100);
  for(auto unm : rg.d_unmRegions) {
    cl.feed(unm);
  }

  for(auto unmCl : cl.d_clusters) {
    emitRegion(jsfp.get(), rg, fastq, gar, "Undermatched", index++, unmCl.getMiddle());
  }
  
  printCorrectMappings(jsfp.get(), rg, "referenceQ");
  if(phix)
    printCorrectMappings(jsfp.get(), *phix, "controlQ");
  fprintf(jsfp.get(), "var qqdata=[");
  bool printedYet=false;
  for(auto coinco = qqcounts.begin() ; coinco != qqcounts.end(); ++coinco) {
    if(coinco->incorrect || coinco->correct) {
      double qscore;
      if(coinco->incorrect && coinco->correct)
	qscore = -10.0*log10(1.0*coinco->incorrect / (coinco->correct + coinco->incorrect));
      else if(coinco->correct == 0)
	qscore=0;
      else
	qscore=41; // "highest score possible"

      fprintf(jsfp.get(), "%s[%ld, %f, %ld]", 
	      printedYet ?  "," : "", 
	      coinco - qqcounts.begin(), qscore,
	      coinco->incorrect + coinco->correct);
      printedYet=true;
    }
  }
  fprintf(jsfp.get(), "];\n");


  uint64_t significantlyVariable=0;
  boost::format fmt1("%-10d: %3d*%c ");
  string fmt2("                  ");
  int aCount, cCount, tCount, gCount;
  double fraction;
  
  struct ClusterLocus
  {
    unsigned int pos;
    ReferenceGenome::LociStats locistat;
    bool operator<(const ClusterLocus& rhs) const
    {
      return pos < rhs.pos;
    }
  };
  vector<ClusterLocus> clusters;

  for(auto& locus : rg.d_locimap) {
    clusters.push_back(ClusterLocus{locus.first, locus.second});
  }

  sort(clusters.begin(), clusters.end());

  Clusterer<ClusterLocus> cll(100);
  for(auto item : clusters) 
    cll.feed(item);

  (*g_log)<<"Found "<<rg.d_locimap.size()<<" varying loci ("<<cll.d_clusters.size()<<" ranges)"<<endl;  

  for(auto& cluster : cll.d_clusters) {
    bool emitted=false;
    for(auto locus : cluster.d_members) {
      unsigned int varcount=variabilityCount(rg, locus.pos, locus.locistat, &fraction);
      if(varcount < 20) 
	continue;
      char c=rg.snippet(locus.pos, locus.pos+1)[0];
      aCount = cCount = tCount = gCount = 0;
      switch(c) {
      case 'A':
	aCount += rg.d_mapping[locus.pos].coverage;
	break;
      case 'C':
	cCount += rg.d_mapping[locus.pos].coverage;
	break;
      case 'T':
	tCount += rg.d_mapping[locus.pos].coverage;
	break;
      case 'G':
	gCount += rg.d_mapping[locus.pos].coverage;
	break;
      }

      if(!emitted) {
	emitRegion(jsfp.get(), rg, fastq, gar, "Variable", index++, locus.pos);
	emitted = true;
      }
      else {
	cerr<<"Skipping emitting other member of cluster "<<locus.pos<<endl;
      }
      cout<< (fmt1 % locus.pos % rg.d_mapping[locus.pos].coverage % rg.snippet(locus.pos, locus.pos+1) ).str();
      sort(locus.locistat.samples.begin(), locus.locistat.samples.end());

      significantlyVariable++;
      for(auto j = locus.locistat.samples.begin(); 
	  j != locus.locistat.samples.end(); ++j) {
	c=get<0>(*j);
	switch(c) {
	case 'A':
	  aCount++;
	  break;
	case 'C':
	  cCount++;
	  break;
	case 'T':
	  tCount++;
	  break;
	case 'G':
	  gCount++;
	  break;
	}
      
	cout<<c;
      }
      cout<<endl<<fmt2;
      for(auto j = locus.locistat.samples.begin(); 
	  j != locus.locistat.samples.end(); ++j) {
	cout<<((char)(get<1>(*j)+33));
      }
      cout<<endl<<fmt2;
      for(auto j = locus.locistat.samples.begin(); 
	  j != locus.locistat.samples.end(); ++j) {
	cout<< (get<2>(*j) ? 'R' : '.');
      }

      int tot=locus.locistat.samples.size() + rg.d_mapping[locus.pos].coverage;
      cout<<endl;
      vector<GeneAnnotation> gas=gar.lookup(locus.pos);
      if(!gas.empty()) {
	cout<<fmt2<<"Annotation: ";
	for(auto& ga : gas) {
	  cout<<ga.name<<" ["<<ga.tag<<"], ";
	}
	cout<<endl;
      }
      cout<<fmt2<< "Fraction tail: "<<fraction<<", "<< locus.locistat.samples.size()<<endl;
      cout<<fmt2<< "A: " << aCount*100/tot <<"%, C: "<<cCount*100/tot<<"%, G: "<<gCount*100/tot<<"%, T: "<<tCount*100/tot<<"%"<<endl;

      cout<<rg.getMatchingFastQs(locus.pos, fastq);
    }
  }
  (*g_log)<<"Found "<<significantlyVariable<<" significantly variable loci"<<endl;
  (*g_log)<<"Found "<<rg.d_insertCounts.size()<<" loci with at least one insert in a read"<<endl;
  struct revsort
  {
    bool operator()(const unsigned int&a, const unsigned int&b) const
    { return a > b;} 
  };
  map<unsigned int, vector<dnapos_t>, revsort> topInserts;
  
  unsigned int significantInserts=0;
  for(const auto& insloc : rg.d_insertCounts) {
    topInserts[insloc.second].push_back(insloc.first);
    if(insloc.second > 10)
      significantInserts++;
  }
  (*g_log)<<"Found "<<significantInserts<<" significant inserts"<<endl;
  
  for(const auto& insert : topInserts) {
    if(insert.first < 10)
      break;
    for(const auto& position : insert.second) {
      cout<<position<<"\t"<<insert.first<<" inserts"<<endl;
      vector<GeneAnnotation> gas=gar.lookup(position);
      if(!gas.empty()) {
	cout<<fmt2<<"Annotation: ";
	for(auto& ga : gas) {
	  cout<<ga.name<<" ["<<ga.tag<<"], ";
	}
	cout<<endl;
      }
      emitRegion(jsfp.get(), rg, fastq, gar, "Insert", index++, position);
      cout<<rg.getMatchingFastQs(position, fastq);
    }
  }
  
  g_log->flush();
  string log = jsonlog.str();
  replace_all(log, "\n", "\\n");
  fprintf(jsfp.get(), "var antonieLog=\"%s\";\n", log.c_str());

  exit(EXIT_SUCCESS);
}

#if 0
  /*
  rg.index(75);
  auto chimericFound=halfFind(unfoundReads, fastq, rg, 75);
  (*g_log)<<(boost::format("Probable chimeric reads:%|40t|-%10d (%.2f%%)\n") 
         % chimericFound % (100.0*chimericFound/total)).str();
  */

  /*
  rg.printFastQs(5718000, fastq);  
  */
  /*
  */
#endif
