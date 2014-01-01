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
#include <assert.h>
#include <time.h>

#include <getopt.h>
#include <dlt/dlt_common.h>

using namespace std;

int verbose = 0;
int max_delta_rel_offset = 4; // max 4s as default. todo add option

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
    Lifecycle() : sec_begin(0), sec_end(0), num_msgs(0), rel_offset_valid(false), min_tmsp(0), max_tmsp(0) {};
    Lifecycle(const DltMessage &);
    void debug_print() const;
    bool fitsin(const DltMessage &); // function is non const. modifies the lifecycle
    // member vars:
    uint32_t sec_begin; // secs since 1.1.1970 for begin of LC
    uint32_t sec_end; // secs since ... for end of LC
    uint32_t num_msgs;
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

/* prototype declarations */
int process_input(std::ifstream &);
int process_message(DltMessage *msg);
int determine_lcs(ECU_Info &);
void debug_print(LIST_OF_LCS &);

MAP_OF_ECUS map_ecus;

int main(int argc, char * argv[])
{
    cout << "dlt-sort (c) 2013, 2014 Matthias Behr\n";
    
    int c, option_index;
    int do_split=0; // by default don't split output files per lifecycle
    
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
                if(verbose) cout << " using <" << optarg << "> as output file name\n";
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
//        assert(info.lcs.size()>0);
        
        char ecu[5];
        ecu[4]=0;
        memcpy(ecu, (char*) &it->first, sizeof(uint32_t));
        cout << "ECU <" << ecu << "> contains " << info.lcs.size() << " lifecycle\n";
        debug_print(info.lcs);
    }
    
    /* determine time-offset between ECUs:
     we have two ways:
     a) by using the abs timestamp within the DltStorageHeader
     b) by using the rel cpu timestamps and some sync markers (Resp/Answer) from ECU to ECU
     */
    
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

void debug_print(LIST_OF_LCS &lcs)
{
    cout << lcs.size() << " lifecycle\n";
    for (LIST_OF_LCS::iterator it = lcs.begin(); it!=lcs.end(); ++it){
        (*it).debug_print();
    }
}

Lifecycle::Lifecycle(const DltMessage &m)
{
    sec_begin = m.storageheader->seconds;
    sec_end = m.storageheader->seconds;
    if (m.headerextra.tmsp){
        min_tmsp =(m.headerextra.tmsp / 10000L) + (m.headerextra.tmsp % 10000 >0 ? 1:0);
        max_tmsp = min_tmsp;
        sec_begin -= min_tmsp; // the lifecycle started at least the cpu runtime before
        rel_offset_valid=true;
    }else{
        rel_offset_valid=false;
        min_tmsp=0;
        max_tmsp=0;
    }
    num_msgs=1;
}

bool Lifecycle::fitsin(const DltMessage &m)
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
        num_msgs++;
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
        
        if (min_tmsp > msg_timestamp) min_tmsp = msg_timestamp;
        if (max_tmsp < msg_timestamp) max_tmsp = msg_timestamp;
        
        num_msgs++;
        return true;
    }else{ // delta too high, lets check whether the start is anyhow within our start/end:
        if (m_abs_lc_starttime <= sec_end && m_abs_lc_starttime >=sec_begin){
        
            if (!rel_offset_valid){
                rel_offset_valid=true;
                delta=0;
            }

            if (min_tmsp > msg_timestamp) min_tmsp = msg_timestamp;
            if (max_tmsp < msg_timestamp) max_tmsp = msg_timestamp;
            
            num_msgs++;
            return true;
        }

    }
    return false;
}

void Lifecycle::debug_print() const
{
    time_t sbeg, send;
    sbeg = sec_begin;
    send = sec_end;
    
    cout << " LC from " << ctime(&sbeg) << "      to ";
    cout << ctime(&send);
    cout << " min_tmsp=" << min_tmsp << " max_tmsp=" << max_tmsp << endl;
    cout << "  num_msgs = " << num_msgs << endl;
}
