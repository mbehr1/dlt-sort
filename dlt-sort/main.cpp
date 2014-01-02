//
//  main.cpp
//  dlt-sort
//
//  Created by Matthias Behr on 31.12.13.
//  Copyright (c) 2013, 2014 Matthias Behr. All rights reserved.
//

#include <iostream>
#include <fstream>
#include <list>
#include <map>
#include <vector>

#include <assert.h>
#include <time.h>

#include <getopt.h>
#include <dlt/dlt_common.h>

using namespace std;

int verbose = 0;
int max_delta_rel_offset = 4; // max 4s as default. todo add option
int max_delta_sec_end_extended = 4; // max 4s. the end get's extended by this message. otherwise we start a new lc. todo add option
int max_delta_sec_begin_earlier = 15; // max 15s. the sec_begin get's adjusted. this msg moves the lc start max 15s earlier. todo add option

void print_usage()
{
    cout << "usage dlt-sort [options] input-file input-file ...\n";
    cout << " -s --split    split output file automatically one for each lifecycle\n";
    cout << " -h --help     show usage/help\n";
    cout << " -v --verbose  set verbose level to 1 (increase by adding more -v)\n";
}

/* type definitions */

typedef std::list<DltMessage *> LIST_OF_MSGS;

class Lifecycle{
public:
    Lifecycle() : sec_begin(0), sec_end(0), rel_offset_valid(false), min_tmsp(0), max_tmsp(0) {};
    Lifecycle(const DltMessage &);
    void debug_print() const;
    bool fitsin(const DltMessage &); // function is non const. modifies the lifecycle
    int64_t calc_min_time() const;
    // member vars:
    uint32_t sec_begin; // secs since 1.1.1970 for begin of LC
    uint32_t sec_end; // secs since ... for end of LC
    LIST_OF_MSGS msgs; // list of messages. we just keep a copy of the ptr to the DltMessage!
    bool rel_offset_valid;
    uint32_t min_tmsp;
    uint32_t max_tmsp;
private:
    bool fitsin_v1(const DltMessage &); // first version working somewhat ok (but strangely because the algorithm is wrong/strange)
};
typedef std::list<Lifecycle> LIST_OF_LCS;

typedef struct{
    LIST_OF_MSGS msgs;
    LIST_OF_LCS lcs;
} ECU_Info;
typedef std::map<uint32_t, ECU_Info> MAP_OF_ECUS;

class OverallLC{
public:
    OverallLC():sec_begin(0), sec_end(0) {};
    OverallLC(const Lifecycle&);
    bool expand_if_intersects(const Lifecycle &);
    bool output_to_fstream(std::ofstream &f);
    void debug_print() const;
    // member vars:
    uint32_t sec_begin;
    uint32_t sec_end;
    
    LIST_OF_LCS lcs; // associated lcs that will be merged into this one.
};
typedef std::list<OverallLC> LIST_OF_OLCS;

typedef struct{
    LIST_OF_MSGS::iterator it;
    LIST_OF_MSGS::iterator end;
    int64_t min_time;
} LC_it;
typedef std::vector<LC_it> VEC_OF_LC_it;

/* prototype declarations */
int process_input(std::ifstream &);
int process_message(DltMessage *msg);
int output_message(DltMessage *msg, std::ofstream &f);
int determine_lcs(ECU_Info &);
int sort_msgs_lcs(ECU_Info &);
void debug_print(const LIST_OF_LCS &);
void debug_print(const LIST_OF_OLCS &);
int determine_overall_lcs();
std::ofstream *get_ofstream(int cnt, std::string &name);


MAP_OF_ECUS map_ecus;
LIST_OF_OLCS list_olcs;

int main(int argc, char * argv[])
{
    cout << "dlt-sort (c) 2013, 2014 Matthias Behr\n";
    
    int c, option_index;
    int do_split=0; // by default don't split output files per lifecycle
    std::string ofilename ("dlt_sorted.dlt");
    
    static struct option long_options[] =
    {
        /* These options set a flag. */
        {"verbose", no_argument,       &verbose, 1},
        // {"brief",   no_argument,       &verbose_flag, 0},
        /* These options don't set a flag.
         We distinguish them by their indices. */
        {"split",     no_argument,       0, 's'},
        {"file",    required_argument, 0, 'f'},
        {"help", no_argument, 0, 'h'},
        {0, 0, 0, 0}
    };
    while ((c = getopt_long (argc, argv, "vhsf:", long_options, &option_index))!= -1){
        switch(c)
        {
            case 0:
                if (long_options[option_index].flag != 0)
                    break;
                printf("option %s", long_options[option_index].name);
                if (optarg)
                    printf(" with arg %s", optarg);
                printf("\n");
                break;
            case 'v':
                verbose++;
                break;
            case 'h':
                print_usage();
                abort();
                break;
            case 's':
                do_split=1;
                if(verbose) cout << " splitting output files by lifecycles\n";
                break;
            case 'f':
                ofilename=std::string (optarg);
                if(verbose) cout << " using <" << ofilename<< "> as output file name\n";
                break;
            default:
                abort();
        }
    }
    argc-=optind;
    argv+=optind;
    
    // print some verbose infos:
    if (verbose){
        cout << " set verbose level to " << verbose << endl;
        cout << " sizeof DltStorageHeader = " << sizeof(DltStorageHeader) << endl;
    }
    
    // let's process the input files:
    for (option_index=0; option_index<argc; option_index++){
        printf("Processing file %s:\n", argv[option_index]);
        std::ifstream fin;
        fin.open(argv[option_index], ios::in);
        if (fin.is_open()){
            
            (void)process_input(fin); // and ignore parsing errors. just continue with next file
            
            fin.close();
        } else {
            cerr << "can't open <" << argv[option_index] << "> as file for input!\n";
            return -1;
        }
    }
    
    // now print some stats:
    // iterate through the list of ECUs:
    for (MAP_OF_ECUS::iterator it=map_ecus.begin(); it!= map_ecus.end(); ++it){
        char ecu[5];
        ecu[4]=0;
        memcpy(ecu, (char*) &it->first, sizeof(uint32_t));
        cout << "ECU <" << ecu << "> contains " << it->second.msgs.size() << " msgs\n";
    }
    
    /* determine lifecycles for each ECU:
     A new lifecycle is determined by the time distance between abs and rel timestamps.
     */
    for (MAP_OF_ECUS::iterator it=map_ecus.begin(); it!= map_ecus.end(); ++it){
        ECU_Info &info = it->second;
        
        determine_lcs(info);
        // now we expect at least one lc!
        assert(info.lcs.size()>0);
        
        sort_msgs_lcs(info);
        
        char ecu[5];
        ecu[4]=0;
        memcpy(ecu, (char*) &it->first, sizeof(uint32_t));
        cout << "ECU <" << ecu << "> contains " << info.lcs.size() << " lifecycle\n";
        debug_print(info.lcs);
    }
    
    /* now determine the set of lifecycles that belong to each other 
     */
    determine_overall_lcs();
    
    // print them:
    if (verbose>0){
        cout << "Overall lifecycles detected (" << list_olcs.size() << ")\n";
        debug_print(list_olcs);
    }
    
    /* now output to file
     if do_split is set we have to maintain a new file for each olc.
     otherwise just output to a single file.
     */
    std::ofstream *f=0;
    int f_cnt=1;
    if (!do_split)
        f=get_ofstream(0, ofilename);
    
    for (LIST_OF_OLCS::iterator it=list_olcs.begin(); it!= list_olcs.end(); ++it){
        if (do_split){
            if (f) f->close();
            f=get_ofstream(f_cnt, ofilename);
        }
        (*it).output_to_fstream(*f); // todo error handling
        ++f_cnt;
    }
    if (f) f->close();
    
    // free memory:
    // tbd!
    
    return 0; // no error (<0 for error)
}

const char DLT_ID4_ID[4] = {'D', 'L', 'T', 0x01};

int process_input(std::ifstream &fin)
{
    int64_t nr_msgs=0;
    // fin is already open and valid
    
    // determine file length:
    fin.seekg(0, fin.end);
    int64_t file_length=fin.tellg();
    fin.seekg(0, fin.beg);

    int64_t remaining = file_length;
    while(remaining>=(int64_t)sizeof(DltStorageHeader)){
        DltMessage *msg=new DltMessage;
        msg->found_serialheader=0;
        msg->resync_offset=0;
        msg->storageheader = (DltStorageHeader *)&msg->headerbuffer[0];
        msg->standardheader = (DltStandardHeader *)&msg->headerbuffer[sizeof(DltStorageHeader)];
        msg->extendedheader = (DltExtendedHeader *)&msg->headerbuffer[sizeof(DltStorageHeader)+sizeof(DltStandardHeader)];
        msg->headersize = -1; // not calculated yet
        msg->datasize=-1; // not calculated yet
        
        // check for dlt storage header:
        fin.read((char*)msg->storageheader, sizeof(*msg->storageheader));
        remaining-= sizeof(*msg->storageheader);
        if (0 == memcmp(msg->storageheader->pattern, DLT_ID4_ID, sizeof(msg->storageheader->pattern))){
            // now read dlt header:
            if (remaining >= sizeof(*msg->standardheader)){
                fin.read((char*)msg->standardheader, sizeof(*msg->standardheader));
                remaining -= sizeof(*msg->standardheader);
                // verify header (somehow)
                uint16_t len = DLT_BETOH_16(msg->standardheader->len); // fixme: why that? the macro should work! DLT_ENDIAN_GET_16(hstd.htyp, hstd.len); // len is without storage header (but with stdh)
                if (len<=sizeof(*msg->standardheader)){
                    cerr << "msg len <= sizeof(DltStandardHeader). Stopping processing this file!\n";
                    remaining = -4;
                }else{
                    len -= sizeof(*msg->standardheader); // standard header already read from this message
                    
                    // extended header?
                    if (DLT_IS_HTYP_WEID(msg->standardheader->htyp))
                    {
                        fin.read(msg->headerextra.ecu, DLT_SIZE_WEID); // DLT_ID_SIZE);
                        remaining -= DLT_SIZE_WEID; // DLT_ID_SIZE;
                        len -= DLT_SIZE_WEID; // DLT_ID_SIZE;
                    }
                    
                    if (DLT_IS_HTYP_WSID(msg->standardheader->htyp))
                    {
                        fin.read((char*)&(msg->headerextra.seid), DLT_SIZE_WSID);
                        msg->headerextra.seid = DLT_BETOH_32(msg->headerextra.seid);
                        remaining -= DLT_SIZE_WSID;
                        len -= DLT_SIZE_WSID;
                    }
                    
                    if (DLT_IS_HTYP_WTMS(msg->standardheader->htyp))
                    {
                        fin.read((char*)&(msg->headerextra.tmsp), DLT_SIZE_WTMS);
                        msg->headerextra.tmsp = DLT_BETOH_32(msg->headerextra.tmsp);
                        remaining -= DLT_SIZE_WTMS;
                        len -= DLT_SIZE_WTMS;
                    } else msg->headerextra.tmsp = 0;
                    
                    // has extended header?
                    if (DLT_IS_HTYP_UEH(msg->standardheader->htyp)){
                        fin.read((char*)msg->extendedheader, sizeof(*msg->extendedheader));
                        remaining -= sizeof(*msg->extendedheader);
                        len -= sizeof(*msg->extendedheader);
                    }
                    
                    // read remaining message:
                    if (remaining >= len){
                        msg->databuffer = new unsigned char [len];
                        msg->databuffersize = len;
                        fin.read((char*)msg->databuffer, len);
                        (void)process_message(msg);
                        // delete msg.databuffer;
                        // delete msg // not necessary. process_message takes care
                        remaining -= len;
                        nr_msgs++;
                    }else{
                        cerr << "truncated message after std header. Stop processing this file!\n";
                        remaining = -3;
                    }
                }
            }else{
                cerr << "no standard header after storage header found! Stop processing this file!\n";
                remaining=-2;
            }
        }else{
            cerr << "no proper DLT pattern found! Stop processing this file!\n";
            cerr << " found: <" << msg->storageheader->pattern[0] << msg->storageheader->pattern[1]
              << msg->storageheader->pattern[2] << msg->storageheader->pattern[3] << ">\n";
            remaining = -1;
        }
        
    }
    if (verbose && remaining!=0) cout << "remaining != 0. parsing errors within that file!\n";
    if (verbose) cout << "processed " << nr_msgs << " msgs\n";
    return (int)remaining; // 0 = success, <0 error in processing
}

int process_message(DltMessage *msg)
{
    // we do sort by:
    // ECU, APID, CTID
    char ecu[DLT_ID_SIZE];
    char apid[DLT_ID_SIZE];
    char ctid[DLT_ID_SIZE];
    uint32_t tmsp=msg->headerextra.tmsp;
    int type=-1;
    
    if (DLT_IS_HTYP_WEID(msg->standardheader->htyp)){
        memcpy(ecu, msg->headerextra.ecu, DLT_ID_SIZE);
    }else{
        memcpy(ecu, msg->storageheader->ecu, DLT_ID_SIZE);
        if (verbose>1) cout << "  using storageheader ecu\n";
    }
    if ((DLT_IS_HTYP_UEH(msg->standardheader->htyp)))// && (msg.extendedheader->apid[0]!=0))
    {
        memcpy(apid, msg->extendedheader->apid, DLT_ID_SIZE);
    }else{
        memset(apid, 0, DLT_ID_SIZE);
        if (verbose>3) cout << "  no apid\n";
    }
    /* extract context id */
    if ((DLT_IS_HTYP_UEH(msg->standardheader->htyp)) )// && (msg.extendedheader->ctid[0]!=0))
    {
        memcpy(ctid, msg->extendedheader->ctid, DLT_ID_SIZE);
    }else{
        memset(ctid, 0, DLT_ID_SIZE);
        if (verbose>3) cout << "  no ctid\n";
    }

    /* extract type */
    if (DLT_IS_HTYP_UEH(msg->standardheader->htyp))
    {
        type = DLT_GET_MSIN_MSTP(msg->extendedheader->msin);
    }
    
    if (verbose>1 && tmsp==0 && (type!=DLT_TYPE_CONTROL)) cout << "  no timestamp on non control msg\n";
    
    // here all data available to sort them:
    LIST_OF_MSGS &list_of_msg = map_ecus[*(uint32_t*)ecu].msgs;
    list_of_msg.push_back(msg);
    
    return 0; // success
}

int output_message(DltMessage *msg, std::ofstream &f)
{
    assert(msg);
    // output storageheader
    f.write((char*)(msg->storageheader), sizeof(*msg->storageheader));
    // output standard header
    f.write((char*)(msg->standardheader), sizeof(*msg->standardheader));
    // output extraheader
    if (DLT_IS_HTYP_WEID(msg->standardheader->htyp))
    {
        f.write(msg->headerextra.ecu, DLT_SIZE_WEID); // DLT_ID_SIZE);
    }
    
    if (DLT_IS_HTYP_WSID(msg->standardheader->htyp))
    {
        msg->headerextra.seid = DLT_HTOBE_32(msg->headerextra.seid);
        f.write((char*)&(msg->headerextra.seid), DLT_SIZE_WSID);
    }
    
    if (DLT_IS_HTYP_WTMS(msg->standardheader->htyp))
    {
        msg->headerextra.tmsp = DLT_HTOBE_32(msg->headerextra.tmsp);
        f.write((char*)&(msg->headerextra.tmsp), DLT_SIZE_WTMS);
    }
    // output extended header
    if (DLT_IS_HTYP_UEH(msg->standardheader->htyp)){
        f.write((char*)msg->extendedheader, sizeof(*msg->extendedheader));
    }
    // output data
    if (msg->databuffersize)
        f.write((char*)msg->databuffer, msg->databuffersize);
    
    return 0; // success
}

int determine_lcs(ECU_Info &ecu)
{
    assert(ecu.lcs.size()==0);
    assert(ecu.msgs.size()>0);
    
    // init with the first message:
    Lifecycle lc(**ecu.msgs.begin());
    ecu.lcs.push_back(lc);
    
    // now go through each message and adjust/insert new lifecycles:
    if (ecu.msgs.size()>1){
        LIST_OF_LCS::iterator cur_l = ecu.lcs.begin();
        LIST_OF_MSGS::iterator it = ecu.msgs.begin();
        ++it; // we skip the first msgs as we treated this already above
        for (; it!=ecu.msgs.end(); ++it){
            // to optimize performance we always check with the last matching (cur_l) one:
            if (!((*cur_l).fitsin(**it))){
                // check whether it fits into any other lifecyle:
                bool found_other=false;
                for(LIST_OF_LCS::iterator lit = ecu.lcs.begin(); !found_other && lit!=ecu.lcs.end(); ++lit){
                    if (lit!=cur_l && (((*lit).fitsin(**it)))){
                        found_other=true;
                        cur_l = lit;
                    }
                }
                // create a new lifecycle based on the msg and set l to this one
                if (!found_other){
                    Lifecycle new_lc(**it);
                    ecu.lcs.push_back(new_lc); // todo we could use a sorted list here
                    cur_l = ecu.lcs.end(); // (if sorted we need to find it in a different way)
                    --cur_l; // end points to a non existing element.
                }
            } // else fits in cur_l -> next msg
        }
    }
    
    return 0; // success
}

bool compare_tmsp(const DltMessage *first, const DltMessage *second)
{
    // should return true if first goes before second in strict weak ordering
    if (first->headerextra.tmsp < second->headerextra.tmsp) return true;
    return false;
}

int sort_msgs_lcs(ECU_Info &ecu)
{
    if (verbose>1) cout << "sorting...\n";
    for (LIST_OF_LCS::iterator it = ecu.lcs.begin(); it!=ecu.lcs.end(); ++it){
        (*it).msgs.sort(compare_tmsp);
    }
    if (verbose>1) cout << "...done\n";
    return 0; // success
}

void debug_print(const LIST_OF_LCS &lcs)
{
    // cout << lcs.size() << " lifecycle\n";
    for (LIST_OF_LCS::const_iterator it = lcs.begin(); it!=lcs.end(); ++it){
        (*it).debug_print();
    }
}

void debug_print(const LIST_OF_OLCS &olcs)
{
    for (LIST_OF_OLCS::const_iterator it=olcs.begin(); it!= olcs.end(); ++it){
        (*it).debug_print();
    }
}

Lifecycle::Lifecycle(const DltMessage &m)
{
    sec_begin = m.storageheader->seconds;
    sec_end = m.storageheader->seconds;
    if (m.headerextra.tmsp){
        min_tmsp = m.headerextra.tmsp;
        max_tmsp = min_tmsp;
        sec_begin -= (m.headerextra.tmsp / 10000L) + (m.headerextra.tmsp % 10000 >0 ? 1:0); // the lifecycle started at least the cpu runtime before
        rel_offset_valid=true;
    }else{
        rel_offset_valid=false;
        min_tmsp=0;
        max_tmsp=0;
    }
    msgs.push_front((DltMessage *)&m); // we might consider removing the const in the param. but by adding it to the list and even within we will never modify the object.
}

bool Lifecycle::fitsin(const DltMessage &m)
{
    /* this is the main function for the whole sorting part
     we check here whether a msg might belong to that lifecycle.
     if yes, we adjust the begin/end of this lifecycle.
     
     The algorithm checks:
     Assuming that this message belongs to this lifecycle:
     a) what would be the absolut starttime of this lifecycle?
     b) what would be the absolut time of this message

     Basically the model behind is:
     An ECU gets started at abs starttime t0 (this would be rel. tmsp 0).
     Each message get's logged at rel time with tmsp x (at abs time tx=t0+x).
     The message takes time j to be processed (due to scheduling/cpu priorites/queues/
     time to connect the logger/... to arrive at the logger and then gets 
     the abs timestamp in the storageheader sh_tx: sh_tx = tx+j = t0+x+j
     There will be a minimal processing time j_min>=0.
     
     The abs starttime of the lifecycle is assumed to be at j_min.

     simplified: t0 = min{from all messages}(sh_tx - x)
    
     The abs time tx of this message is then: t0 + x. This varies over time as 
     the minimum will shrink over time. So ideally a two path approach would be 
     needed.
     
     
     For now we ignore/throw away msgs with rel. tmsp 0 as they are effectively send
     at a different time and are thus unreliable.
     todo: simply add those messages if they fit by the sh_tx time within that lifecycle.
     
     todo: add support for abs tmsp (vs. rel. as required by dlt spec) as this seems to be 
      used by some ECU dlt implementations...
     
     */
    
    // if tmsp is 0 we claim it fits but actually throw it away (see above)
    if (m.headerextra.tmsp==0){
        // msgs.push_back((DltMessage *)&m);
        return true;
    }
    
    // msg tmsp in seconds (rounded) (x from above)
    uint32_t msg_timestamp = (m.headerextra.tmsp / 10000L) + (m.headerextra.tmsp % 10000 >0 ? 1:0);
    // this would be the starttime if the processing time/jitter j would be 0. (sh_tx-x)
    uint32_t m_abs_lc_starttime = m.storageheader->seconds - msg_timestamp;

    // sec_begin keeps the min abs starttime t0
    // so if this message belongs to this lifecycle there would be
    // new_sec_begin = min(sec_begin, m_abs_lc_starttime)
    // the message would have been send at: m_tx = sec_begin + msg_timestamp.
    // if this fits clearly within the end range then we assume the message belongs to this one
    // and update sec_begin accordingly
    // we need to ranges to judge:
    // a) the end get's extended by this message: max_delta_sec_end_extended
    // b) the sec_begin get's adjusted: max_delta_sec_begin_earlier
    
    uint32_t new_sec_begin = min(sec_begin, m_abs_lc_starttime);
    uint32_t m_tx = new_sec_begin + msg_timestamp;
    
    if (((new_sec_begin + max_delta_sec_begin_earlier) >= sec_begin) &&
        (m_tx <= (sec_end+max_delta_sec_end_extended))){
        // ok belongs to this one.
        if (new_sec_begin<sec_begin) sec_begin = new_sec_begin;
        if (m_tx > sec_end) sec_end = m_tx;
        msgs.push_back((DltMessage *)&m);
        
        if (min_tmsp > m.headerextra.tmsp) min_tmsp = m.headerextra.tmsp;
        if (max_tmsp < m.headerextra.tmsp) max_tmsp = m.headerextra.tmsp;
        
        return true;
    }
    
    return false;
}


bool Lifecycle::fitsin_v1(const DltMessage &m)
{
    /* this is the main function for the whole sorting part
     we check here whether a msg might belong to that lifecycle.
     if yes, we adjust the begin/end of this lifecycle.
     We need to check:
      a) begin/end close to current one (??? what's close))
      b) rel_offset similar (<= max_delta_rel_offset)
     todo adjust description!
     */
    
    // determine rel_offset
    
    // if tmsp is 0 we put it into this one but ignore the sec_begin/end:
    if (m.headerextra.tmsp==0){
        msgs.push_back((DltMessage *)&m);
        return true;
    }
    
    uint32_t msg_timestamp = (m.headerextra.tmsp / 10000L) + (m.headerextra.tmsp % 10000 >0 ? 1:0);
    uint32_t m_abs_lc_starttime = m.storageheader->seconds - msg_timestamp; // tmsp is in 0.1ms
    
    int delta=0;
    if (rel_offset_valid) {
        if (m_abs_lc_starttime>=sec_begin)
            delta = m_abs_lc_starttime - sec_begin;
        else
            delta = sec_begin - m_abs_lc_starttime;
    }
    
    if (delta <= max_delta_rel_offset){
        
        // adjust begin? -> we can't. we merge the lifecycles later in another step.
        
        if (sec_begin > m_abs_lc_starttime)
            sec_begin = m_abs_lc_starttime; // if we do so the rel_offset should change as well!?
        
        
        // adjust end?
        if (sec_end < m.storageheader->seconds)
            sec_end = m.storageheader->seconds;
        
        if (!rel_offset_valid){
            rel_offset_valid=true;
            delta=0;
        }
        
        if (min_tmsp > m.headerextra.tmsp) min_tmsp = m.headerextra.tmsp;
        if (max_tmsp < m.headerextra.tmsp) max_tmsp = m.headerextra.tmsp;
        
        msgs.push_back((DltMessage *)&m);
        return true;
    }else{ // delta too high, lets check whether the start is anyhow within our start/end:
        if (m_abs_lc_starttime <= sec_end && m_abs_lc_starttime >=sec_begin){
        
            if (!rel_offset_valid){
                rel_offset_valid=true;
                delta=0;
            }

            if (min_tmsp > m.headerextra.tmsp) min_tmsp = m.headerextra.tmsp;
            if (max_tmsp < m.headerextra.tmsp) max_tmsp = m.headerextra.tmsp;
            
            msgs.push_back((DltMessage *)&m);
            return true;
        }

    }
    return false;
}

int64_t Lifecycle::calc_min_time() const
{
    int64_t ret=0;
    // return the min (i.e. from first msg) abs time (in 0.1ms):
    ret=sec_begin;
    ret *=10000L;
    
    ret-=min_tmsp;
    
    // now add the time from the first msg:
    if (msgs.size()){
        DltMessage *m = *msgs.begin();
        ret+=m->headerextra.tmsp;
    }
    
    return ret;
}

void Lifecycle::debug_print() const
{
    time_t sbeg, send;
    sbeg = sec_begin;
    send = sec_end;
    
    cout << " LC from " << ctime(&sbeg) << "      to ";
    cout << ctime(&send);
    cout << " min_tmsp=" << min_tmsp << " max_tmsp=" << max_tmsp << endl;
    cout << "  num_msgs = " << msgs.size() << endl;
}

OverallLC::OverallLC(const Lifecycle &lc)
{
    sec_begin=lc.sec_begin;
    sec_end=lc.sec_end;
    lcs.push_back(lc);
}

void OverallLC::debug_print() const
{
    time_t sbeg, send;
    sbeg = sec_begin;
    send = sec_end;
    
    cout << " LC from " << ctime(&sbeg) << "      to ";
    cout << ctime(&send);
    cout << "  num_lcs = " << lcs.size() << endl;
}


bool OverallLC::expand_if_intersects(const Lifecycle &lc)
{
    if (lc.sec_begin > sec_end) return false;
    if (lc.sec_end < sec_begin) return false;
    // ok, we intersect. do we need to expand?
    if (lc.sec_begin<sec_begin) {
        sec_begin = lc.sec_begin;
        lcs.push_front(lc);
    }else{
        lcs.push_back(lc);
    }
    if (lc.sec_end>sec_end) sec_end = lc.sec_end;
    
    return true;
}

bool OverallLC::output_to_fstream(std::ofstream &f)
{
    /* this is the main function to output/merge the different lifecycles
     belonging to an overall lifecycle.
     The lifecycles itself are sorted already but they have different 
     initial offsets. The time-offset between ECUs is determined by:
     lc sec_begin minus min_tmsp plus the timestamp
     */
    
    assert(f.is_open());
    
    /* for each lifecycle/associated msg list we keep in a vector
     iterator current and end
     next_time in 0.1ms (timestamp resolution) */
    VEC_OF_LC_it vec;
    
    /* then we always keep: index, next_index, next_time
     and output the msgs from vector[index] till >next_time
     then we determine new next_time and next index
     and keep on looping until next_index is empty (vector empty)
     */

    LC_it *index=NULL; // pointing to the LC_it with the min.time
    LC_it *next_index=NULL; // pointing to the LC_it with the min. time not from elem index
    int64_t next_time; // next time where the change from index to next_index has to happen
    
    int64_t min_time=0;
    for (LIST_OF_LCS::iterator it = lcs.begin(); it!=lcs.end(); ++it){
        LC_it l;
        l.it = (*it).msgs.begin();
        l.end = (*it).msgs.end();
        l.min_time= (*it).calc_min_time();
        vec.push_back(l);
        /* we can't determine index here as adding elements might invalidate the ptr to the elements!
        if (!index){
            min_time=l.min_time;
            index=&vec[vec.size()-1];
        }else{
            if (l.min_time < min_time){
                min_time = l.min_time;
                index=&vec[vec.size()-1];
            }
        } */
    }
    while(vec.size()>1){ // as long as we have at least two msg lists to take care of:
        while (next_index==NULL){
            // need to determine the next index and next time to stop at:
            for (VEC_OF_LC_it::iterator i=vec.begin(); i!=vec.end(); ++i){
                if (index!=&(*i)){ // don't search at the current one
                    if (!next_index){
                        next_time=(*i).min_time;
                        next_index = &(*i);
                    }else{
                        if ((*i).min_time < next_time){
                            next_time = (*i).min_time;
                            next_index = &(*i);
                        }
                    }
                }
            }
            assert(next_index);
            assert(next_time>=min_time); // otherwise we did a logic error
            if (index==NULL){ // we reuse this to determine index as well
                // this works as the comparision above is against NULL in that case
                index=next_index;
                min_time=next_time;
                next_index=0;
            }
        }
        assert(index);
        assert(next_index);
        assert(next_time>=min_time);
        
        // now output msgs from index until time >next time:
        do{
            uint32_t tmsp = (*(index->it))->headerextra.tmsp; // this needs to be done before output_message!
            output_message(*(index->it), f); // see below!
            ++(index->it);
            if (index->it != index->end){
                // update LC_it
                index->min_time -= tmsp;
                index->min_time += (*(index->it))->headerextra.tmsp;
            }else{
                // emptied this lc! we need to delete this element
                for (VEC_OF_LC_it::iterator j=vec.begin(); (NULL!=index) && (j!=vec.end()); ++j){
                    if (index==&(*j)){
                        vec.erase(j);
                        index=NULL;
                    }
                }
                assert(NULL==index);
                // sadly we need to reset next_index here as well
                // as the vector erase might make it invalid!
                next_index=NULL;
            }
            
        }while((NULL!=index) && (index->min_time<=next_time));
        
        // now use the next one, except if the index one is empty.
        // in that case rearrange the vector
        index=next_index;
        min_time=next_time;
        
        // need to set next_index to zero to be reset at next iteration:
        next_index=NULL;
    }
    
    while(vec.size()>0){ // just one item remaining:
        assert(vec.size()==1);
        // todo output the msgs:
        LC_it &l=vec[0];
        for(; l.it!=l.end; ++l.it){
            output_message(*(l.it), f); // take care. after this call the message is corrupt! (endianess...)
        }
        vec.erase(vec.begin());
    }
    
    return true; // success
}

int determine_overall_lcs()
{
    assert(list_olcs.size()==0);
    
    // populate list_olcs with the merged/intersected lcs from map_ecus
    for (MAP_OF_ECUS::iterator it=map_ecus.begin(); it!= map_ecus.end(); ++it){
        ECU_Info &info = it->second;
        for (LIST_OF_LCS::iterator lit=info.lcs.begin(); lit!=info.lcs.end(); ++lit){
            bool found_intersect=false;
            // iterate throug all olcs: this has O(n^2) complexity but we expect always very few lcs
            for(LIST_OF_OLCS::iterator oit=list_olcs.begin(); !found_intersect && (oit!=list_olcs.end()); ++oit){
                if ((*oit).expand_if_intersects(*lit))
                    found_intersect=true;
            }
            // if not found then add new one:
            if (!found_intersect){
                OverallLC nlc((*lit));
                list_olcs.push_front(nlc);
            }
        }
    }
    return 0; // success
}

std::ofstream *get_ofstream(int cnt, std::string &templ)
{
    std::string name(templ);
    if (cnt>0){
        // we have to add _xxx to the filename
        char nr[20];
        snprintf(nr, 19, "%03d", cnt); // todo as we now the max number upfront we could just use an many digits as needed
        name.append(nr);
        // but before the ".dlt"
        name.append(".dlt");
    }
    // now open the file:
    std::ofstream *f=new std::ofstream;
    f->open(name, ios_base::out | ios_base::binary | ios_base::trunc);
    if (!(f->is_open())){
        delete f;
        f=NULL;
        cerr << "can't open <" << name << "> for writing! Aborting!";
        abort();
    }
    return f;
}
