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
#include <algorithm> // for min
#include <iostream>
#include <fstream>
#include <string>
#include <list>
#include <map>
#include <vector>

#include <dlt/dlt_common.h>

const long usecs_per_sec = 1000000L;
extern int verbose;

/* type definitions */

typedef std::list<DltMessage *> LIST_OF_MSGS;

class Lifecycle{
public:
    Lifecycle() : usec_begin(0), usec_end(0), rel_offset_valid(false), min_tmsp(0), max_tmsp(0) {};
    Lifecycle(const DltMessage &);
    void debug_print() const;
    bool fitsin(const DltMessage &); // function is non const. modifies the lifecycle
    int64_t calc_min_time() const;
    bool expand_if_intersects(Lifecycle &l);
    // member vars:
    int64_t usec_begin; // secs since 1.1.1970 for begin of LC
    int64_t usec_end; // secs since ... for end of LC
    LIST_OF_MSGS msgs; // list of messages. we just keep a copy of the ptr to the DltMessage!
    bool rel_offset_valid;
    uint32_t min_tmsp;
    uint32_t max_tmsp;
    
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
int sort_msgs_lcs(ECU_Info &);
int merge_lcs(ECU_Info &);

void debug_print(const LIST_OF_LCS &);
void debug_print(const LIST_OF_OLCS &);
int determine_overall_lcs();
std::ofstream *get_ofstream(int cnt, std::string &name);

extern MAP_OF_ECUS map_ecus;
extern LIST_OF_OLCS list_olcs;

#endif
