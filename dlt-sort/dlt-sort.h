//
//  dlt-sort.h
//  dlt-sort
//
//  Created by Matthias Behr on 11.01.14.
//  Copyright (c) 2014 Matthias Behr. All rights reserved.
//

#ifndef dlt_sort_dlt_sort_h
#define dlt_sort_dlt_sort_h

#include <assert.h>
#include <time.h> // for ctime
#include <string.h> // for memcpy and snprintf
#include <stdlib.h> // for abort
#include <algorithm> // for min
#include <iostream>
#include <fstream>
#include <string>
#include <list>
#include <map>
#include <vector>

#include <dlt/dlt_common.h>

#ifdef WIN32 // M$ doesnt seem to like (yet) snprintf
#define snprintf _snprintf_s
#endif

const int64_t usecs_per_sec = 1000000;
const int64_t usecs_per_tmsp = 100;

// the header versions supported: (currently just 1)
const int DLT_HEADER_VERSION_MIN = 1;
const int DLT_HEADER_VERSION_MAX = 1;

extern int verbose;
extern int trust_logger_time;
extern int use_max_earlier_sanity_check;
extern int64_t max_earlier_begin_usec;

/* type definitions */

typedef std::list<DltMessage *> LIST_OF_MSGS;

class Lifecycle{
public:
    Lifecycle() : usec_begin(0), usec_end(0), rel_offset_valid(false), min_tmsp(0), max_tmsp(0), clock_skew(1.0f) {};
    Lifecycle(const DltMessage &);
    void debug_print() const;
    bool fitsin(const DltMessage &); // function is non const. modifies the lifecycle
    void set_clock_skew(double new_skew); // non const! adjusts even usec_begin, usec_end
    int64_t calc_min_time() const;
    int64_t determine_max_latency(int64_t begin=-1, double skew=-1.0) const;
    double determine_clock_skew() const;
    int64_t determine_begin (double skew) const;
    int64_t determine_end () const;
    bool expand_if_intersects(Lifecycle &l);
    // member vars:
    int64_t usec_begin; // secs since 1.1.1970 for begin of LC
    int64_t usec_end; // secs since ... for end of LC
    LIST_OF_MSGS msgs; // list of messages. we just keep a copy of the ptr to the DltMessage!
    bool rel_offset_valid;
    uint32_t min_tmsp;
    uint32_t max_tmsp;
    // clock skew support:
    double clock_skew;
    
};
typedef std::list<Lifecycle> LIST_OF_LCS;

typedef struct{
    LIST_OF_MSGS msgs;
    LIST_OF_LCS lcs;
} ECU_Info;
typedef std::map<uint32_t, ECU_Info> MAP_OF_ECUS;

class OverallLC{
public:
    OverallLC():usec_begin(0), usec_end(0) {};
    OverallLC(const Lifecycle&);
    bool expand_if_intersects(const Lifecycle &);
    bool output_to_fstream(std::ofstream &f, bool timeadjust);
    void debug_print() const;
    // member vars:
    int64_t usec_begin;
    int64_t usec_end;
    
    LIST_OF_LCS lcs; // associated lcs that will be merged into this one.
};
typedef std::list<OverallLC> LIST_OF_OLCS;

typedef struct{
    LIST_OF_MSGS::iterator it;
    LIST_OF_MSGS::iterator end;
    int64_t min_time;
    int64_t usec_begin;
} LC_it;
typedef std::vector<LC_it> VEC_OF_LC_it;

/* prototype declarations */
void init_DltMessage(DltMessage &);
int process_input(std::ifstream &);
int process_message(DltMessage *msg);
int output_message(DltMessage *msg, std::ofstream &f);
int determine_lcs(ECU_Info &);
void determine_clock_skew(ECU_Info &);
bool compare_tmsp(const DltMessage *first, const DltMessage *second);
int sort_msgs_lcs(ECU_Info &);
bool compare_usecbegin(const OverallLC &first, const OverallLC &second);
int merge_lcs(ECU_Info &);

void debug_print(const LIST_OF_LCS &);
void debug_print(const LIST_OF_OLCS &);
void debug_print_message(const DltMessage &msg);
int determine_overall_lcs();
std::string get_ofstream_name(int cnt, std::string const &templ);
std::ofstream *get_ofstream(int cnt, std::string const &name);

extern MAP_OF_ECUS map_ecus;
extern LIST_OF_OLCS list_olcs;

#endif
