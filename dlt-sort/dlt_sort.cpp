//
//  dlt_sort.cpp
//  dlt-sort
//
//  Created by Matthias Behr on 11.01.14.
//  Copyright (c) 2014 Matthias Behr. All rights reserved.
//

#include <iomanip>
#include <limits>
#include "dlt-sort.h"

using namespace std;

int verbose = 0;
int trust_logger_time=0; // by default we don't trust the logger time. see ::fitsin for an example why
int use_max_earlier_sanity_check=1; // by default enabled.
int64_t max_earlier_begin_usecs = 120ll*usecs_per_sec; // by default max 2mins
int use_clock_drift_detection=1; // by default enabled.

MAP_OF_ECUS map_ecus;
LIST_OF_OLCS list_olcs;

void init_DltMessage(DltMessage &msg){
    msg.found_serialheader=0;
    msg.resync_offset=0;
    msg.storageheader = (DltStorageHeader *)&msg.headerbuffer[0];
    msg.standardheader = (DltStandardHeader *)&msg.headerbuffer[sizeof(DltStorageHeader)];
    msg.extendedheader = (DltExtendedHeader *)&msg.headerbuffer[sizeof(DltStorageHeader)+sizeof(DltStandardHeader)];
    msg.headersize = -1; // not calculated yet
    msg.datasize=-1; // not calculated yet
}

Lifecycle::Lifecycle(const DltMessage &m)
{
    clock_skew = 1.0f;
    usec_begin = m.storageheader->seconds;
    usec_begin *= usecs_per_sec;
    usec_begin += m.storageheader->microseconds;
    usec_end = usec_begin;
    if (m.headerextra.tmsp){
        min_tmsp = m.headerextra.tmsp;
        max_tmsp = min_tmsp;
        usec_begin -= (((int64_t)m.headerextra.tmsp) * usecs_per_tmsp); // tmsp is in 0.1ms granularity. The lifecycle started at least the cpu runtime before
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
     
     
     */
    bool toret = false;
    // if tmsp is 0 we claim it fits but actually throw it away (see above)
    if (m.headerextra.tmsp==0){
        // msgs.push_back((DltMessage *)&m);
        return true;
    }
    
    // msg tmsp in seconds (rounded) (x from above)
    int64_t msg_timestamp = (((int64_t)m.headerextra.tmsp) *usecs_per_tmsp);
    // this would be the starttime if the processing time/jitter j would be 0. (sh_tx-x)
    int64_t m_abs_lc_starttime = (((int64_t)m.storageheader->seconds) * usecs_per_sec)
    + (m.storageheader->microseconds) - msg_timestamp;
    
    // sec_begin keeps the min abs starttime t0
    // so if this message belongs to this lifecycle there would be
    // new_sec_begin = min(sec_begin, m_abs_lc_starttime)
    // the message would have been send at: m_tx = sec_begin + msg_timestamp.
    // if the message fits clearly we and update sec_begin and sec_end accordingly
    
    int64_t new_usec_begin = min(usec_begin, m_abs_lc_starttime);
    // we don't need this any longer int64_t m_new_tx = new_usec_begin + msg_timestamp;
    int64_t m_org_tx = m_abs_lc_starttime + msg_timestamp;
    
    // the m_abs_lc_starttime can only be ge (>=) the real abs_lc_starttime as
    // the jitter can only be positive.
    // so if the m_abs_lc_starttime is within sec_begin and sec_end we can trust
    // this message:
    
    // so this is a 100% safe check (uncorrelated start time within the current lifecycle)
    if ((m_abs_lc_starttime >= usec_begin) && (m_abs_lc_starttime <= usec_end))
    {
        toret = true;
    }
    
    // another check:
    // if the begin is before the current end
    // but the msg tx time is after our begin (by this we avoid to misdetect
    // lifecycles directly before the current one)
    // we treat it as part of this one
    
    // so this is a 100% safe check as well (uncorrelated start before end but uncor. tx within current lc)
    if (!toret
        && (m_abs_lc_starttime <= usec_end)
        && (m_org_tx >= usec_begin))
    {
        if (use_max_earlier_sanity_check){
            // I thought it's a 100% safe check but we seem to have to instances with corrupted timestamps
            // this sometimes moves the begin of a lifecycle completely to a lot earlier.
            // so we add a max delta check here:
            if ((new_usec_begin + max_earlier_begin_usecs) > usec_begin)
                toret = true;
            else{
                if (verbose>=1) debug_print_message(m);
                cerr << "! max_earlier_begin_usecs check failed! (tmsp " << m.headerextra.tmsp << " corrupt?. Ignoring this msg!\n";
                return true; // mark it as fitting into this lifecycle but ignoring the message!
            }
        }else{
            toret = true;
        }
    }
    
    if (toret){
        // ok belongs to this one.
        if (new_usec_begin<usec_begin) usec_begin = new_usec_begin;
        
        if (trust_logger_time) { // due to some logger faults we can't trust the m_org_tx time
            // to determine/extend the end of the lifecycle we can use the
            // abs time when the message was received by the logger as
            // processing jitter can not occur between lifecycles
            if (m_org_tx>usec_end)
                usec_end = m_org_tx;
        }else{
            // to determine/extend the end of the lifecycle we use the
            // usec_begin (probably adjusted by this msg) and the
            // timestamp from this message. I.e. we don't trust the logger time
            // this will lead to more lifecycles detected but that's better
            // than detecting them wrongly in the previous lifecycle
            // example:
            // [ECUX] 7189760 2014/01/17 18:20:03.924584 54 40 16
            // [ECUX] 7189790 2014/01/17 18:20:06.968501 54 38 18 <- logger wrongly uses the timestamp of the next msg (or first char of the next msg) here.
            // [ECUX]      50 2014/01/17 18:20:06.968515 54 0 20
            int64_t m_new_tx = usec_begin + msg_timestamp;
            if (m_new_tx > usec_end)
                usec_end = m_new_tx;

        }
        
        msgs.push_back((DltMessage *)&m);
        
        // update min/max tmsp:
        if (!rel_offset_valid){
            min_tmsp = m.headerextra.tmsp;
            rel_offset_valid = true;
        }else{
            if (min_tmsp > m.headerextra.tmsp) min_tmsp = m.headerextra.tmsp;
        }
        if (max_tmsp < m.headerextra.tmsp) max_tmsp = m.headerextra.tmsp;
    }
    
    return toret;
}


int64_t Lifecycle::calc_min_time() const
{
    int64_t ret=0;
    // return the min (i.e. from first msg) abs time (in 0.1ms):
    ret=usec_begin;
    
    // this is already anticipated in usec_begin ret-=(((int64_t)min_tmsp) * 100L); // tmsp in 0.1ms gran.
    
    // now add the time from the first msg:
    if (msgs.size()){
        DltMessage *m = *msgs.begin();
        ret+=multiply(((int64_t)m->headerextra.tmsp) * usecs_per_tmsp, clock_skew);
    }
    
    return ret;
}

int64_t Lifecycle::determine_max_latency(int64_t begin, double skew) const
{
    int64_t ret=0;
    if (begin==-1) begin = usec_begin;
    if (skew<0.0) skew = clock_skew;
    
    // latency is the difference between usec_begin+tmsp and storageheader time
    // here we determine the maximum latency from all msgs
    for(LIST_OF_MSGS::const_iterator it=msgs.begin(); it!=msgs.end(); ++it){
        DltMessage *m = *it;
        int64_t latency=(usecs_per_sec*m->storageheader->seconds)+m->storageheader->microseconds;
        latency -= begin;
        latency -= multiply((usecs_per_tmsp*m->headerextra.tmsp), skew); // we don't use the internal clock_skew here
        if (latency<0) return latency; // error, return it
        if (latency>ret) ret=latency;
    }
    return ret;
}

int64_t Lifecycle::determine_begin (double skew) const
{
    int64_t ret=std::numeric_limits<int64_t>::max();
    // what would the new begin be if we had a clock skew?
    for(LIST_OF_MSGS::const_iterator it=msgs.begin(); it!=msgs.end(); ++it){
        DltMessage *m = *it;
        int64_t begin=(usecs_per_sec*m->storageheader->seconds)+m->storageheader->microseconds;
        begin -= multiply(usecs_per_tmsp*m->headerextra.tmsp, skew);
        if (begin<ret) ret=begin;
    }
    return ret;
}

int64_t Lifecycle::determine_end() const
{
    int64_t ret = multiply(max_tmsp, clock_skew);
    ret += usec_begin;
    return ret;
}

void determine_clock_skew(ECU_Info &ecu)
{
    if (ecu.lcs.size()==0) return;
    if (ecu.msgs.size()==0) return;

    double skew_min = 0.5;
    double skew_max = 1.5;
    double r_skew=1.25; // >1 = ECU clock is faster than logger clock
    double l_skew = 0.75;
    // do a binary search with skew. between 0.5 and 1.5, 1.25|1.75,...
    // at 1 is known = determine_max_latency()
    // optimize with max_latency so that it's >=0 but as small as possible
    
    // 1. determine max_latency for all lcs at skew 1.0:
    int64_t last_lat = 0;
    for (LIST_OF_LCS::const_iterator it=ecu.lcs.begin(); it!=ecu.lcs.end(); ++it){
        int64_t lat = (*it).determine_max_latency();
        if (lat>last_lat)
            last_lat = lat;
    }
    double last_skew = 1.0;
    int i=20; // iterations
    do{
        int64_t r_lat=0;
        for (LIST_OF_LCS::const_iterator it=ecu.lcs.begin(); it!=ecu.lcs.end(); ++it){
            int64_t r_beg = (*it).determine_begin(r_skew);
            int64_t t_lat = (*it).determine_max_latency(r_beg, r_skew);
            if (t_lat<0){
                r_lat = t_lat;
                break;
            }
            if (t_lat>r_lat)
                r_lat = t_lat;
        }

        if (r_lat<0){
            // this direction is invalid!
            skew_max=r_skew;
        }
        
        int64_t l_lat=0;
        for (LIST_OF_LCS::const_iterator it=ecu.lcs.begin(); it!=ecu.lcs.end(); ++it){
            int64_t l_beg = (*it).determine_begin(l_skew);
            int64_t t_lat = (*it).determine_max_latency(l_beg, l_skew);
            if (t_lat<0){
                l_lat = t_lat;
                break;
            }
            if (t_lat>l_lat)
                l_lat = t_lat;
        }
        if (l_lat<0){
            skew_min = l_skew;
        }
        if (verbose>=3)cout << l_skew << "=" << l_lat << " " << r_skew << "=" << r_lat << endl;

        // now decide which direction is better:
        if (r_lat<0 && l_lat<0){
            r_skew = last_skew+skew_max;
            r_skew /=2.0;
            l_skew = last_skew+skew_min;
            l_skew /=2.0;
        }else{
            // not both dirs invalid:
            if (l_lat<0){
                l_skew = last_skew + skew_min;
                l_skew /=2.0;
            }else{
                // l_lat ok. better?
                if (l_lat < last_lat){
                    // good direction:
                    last_skew = l_skew;
                    last_lat = l_lat;
                    l_skew = last_skew + skew_min;
                    l_skew /=2.0;
                }else{
                    skew_min = l_skew;
                    l_skew = last_skew + skew_min;
                    l_skew /= 2.0;
                }
            }
            if (r_lat<0){
                r_skew = last_skew + skew_max;
                r_skew /=2.0;
            }else{
                // r_lat ok. better?
                if (r_lat < last_lat){
                    // good direction:
                    last_skew = r_skew;
                    last_lat = r_lat;
                    r_skew = last_skew + skew_max;
                    r_skew /=2.0;
                }else{
                    skew_max = r_skew;
                    r_skew = last_skew + skew_max;
                    r_skew /=2.0;
                }
            }
        }
        --i;
    }while(i>0);
    // last_skew contains the optimum:
    // adjust each lc:
    //cout << endl;
    for (LIST_OF_LCS::iterator it=ecu.lcs.begin(); it!=ecu.lcs.end(); ++it){
        (*it).set_clock_skew(last_skew);
        //cout << " max latency = " << (*it).determine_max_latency() << endl;
    }

    if (verbose>=1) cout << "\npossible clock skew = " << ((last_skew-1.0f)*100.0) << "%" << endl;

}

double Lifecycle::determine_clock_skew() const
{
    double skew_min = 0.5;
    double skew_max = 1.5;
    double r_skew=1.25; // <1 = ECU clock is faster than logger clock (we need to slow down the tmsps)
    double l_skew = 0.75;
 
    
    // do a binary search with skew. between 0.5 and 1.5, 1.25|1.75,...
    // at 1 is known = determine_max_latency()
    // optimize with max_latency so that it's >=0 but as small as possible
    //assert(determine_begin(1.0)==usec_begin);
    int64_t last_beg = usec_begin;
    int64_t last_lat = determine_max_latency(last_beg);
    if (verbose >=2)cout << endl << "1.0 =" << last_lat << endl;
    double last_skew = 1.0;
    int i=20; // iterations
    do{
        int64_t r_beg = determine_begin(r_skew);
        int64_t r_lat = determine_max_latency(r_beg, r_skew);
        if (r_lat<0){
            // this direction is invalid!
            skew_max=r_skew;
        }
        int64_t l_beg = determine_begin(l_skew);
        int64_t l_lat = determine_max_latency(l_beg, l_skew);
        if (l_lat<0){
            skew_min = l_skew;
        }
        if (verbose>=3)cout << l_skew << "=" << l_lat << " " << r_skew << "=" << r_lat << endl;
        // now decide which direction is better:
        if (r_lat<0 && l_lat<0){
            r_skew = last_skew+skew_max;
            r_skew /=2.0;
            l_skew = last_skew+skew_min;
            l_skew /=2.0;
        }else{
            // not both dirs invalid:
            if (l_lat<0){
                l_skew = last_skew + skew_min;
                l_skew /=2.0;
            }else{
                // l_lat ok. better?
                if (l_lat < last_lat){
                    // good direction:
                    last_skew = l_skew;
                    last_lat = l_lat;
                    l_skew = last_skew + skew_min;
                    l_skew /=2.0;
                }else{
                    skew_min = l_skew;
                    l_skew = last_skew + skew_min;
                    l_skew /= 2.0;
                }
            }
            if (r_lat<0){
                r_skew = last_skew + skew_max;
                r_skew /=2.0;
            }else{
                // r_lat ok. better?
                if (r_lat < last_lat){
                    // good direction:
                    last_skew = r_skew;
                    last_lat = r_lat;
                    r_skew = last_skew + skew_max;
                    r_skew /=2.0;
                }else{
                    skew_max = r_skew;
                    r_skew = last_skew + skew_max;
                    r_skew /=2.0;
                }
            }
        }
        --i;
    }while(i>0);
    
    
    return last_skew;
}

void Lifecycle::set_clock_skew(double new_skew)
{
    usec_begin = determine_begin(new_skew);
    clock_skew = new_skew;
    usec_end = determine_end();
}

bool Lifecycle::expand_if_intersects(Lifecycle &lc)
{
    if (lc.usec_begin > usec_end) return false;
    if (lc.usec_end < usec_begin) return false;
    // ok, we intersect. do we need to expand?
    if (lc.usec_begin<usec_begin) usec_begin = lc.usec_begin;
    if (lc.usec_end>usec_end) usec_end = lc.usec_end;
    
    if (lc.rel_offset_valid && (lc.min_tmsp < min_tmsp)) min_tmsp = lc.min_tmsp;
    if (lc.max_tmsp > max_tmsp) max_tmsp = lc.max_tmsp;
    
    // now move all the messages from lc to us:
    msgs.splice(msgs.begin(), lc.msgs); // take care we loose sorting here (if it was sorted before!)
    
    return true;
}


void Lifecycle::debug_print() const
{
    time_t sbeg, send;
    sbeg = usec_begin / usecs_per_sec;
    send = usec_end / usecs_per_sec;
    
    cout << " LC from " << ctime(&sbeg) << "      to ";
    cout << ctime(&send);
    cout << "  min_tmsp=" << min_tmsp << " max_tmsp=" << max_tmsp << endl;
    cout << "  num_msgs = " << msgs.size() << endl;
    if (verbose>=1)
        cout << "  max latency = " << determine_max_latency() << endl;
    // cout << "  possible clock skew = " << ((determine_clock_skew()-1.0f)*100.0) << "%" << endl;
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
        init_DltMessage(*msg);
        
        // read until dlt pattern (DLT0x01) is found:
        bool found_pattern=false;
        ID4 pattern;
        fin.read(pattern, sizeof(ID4));
        remaining -= sizeof(ID4);
        int64_t skipped_bytes = 0;
        do{
            found_pattern = (0 == memcmp(pattern, DLT_ID4_ID, sizeof(pattern)));
            if (!found_pattern){
                // read one byte and shift the current by one byte:
                pattern[0] = pattern[1];
                pattern[1] = pattern[2];
                pattern[2] = pattern[3];
                fin.read(&pattern[3], 1);
                remaining -= 1;
                skipped_bytes++;
            }
        }while (!found_pattern && remaining>=static_cast<int64_t>(((int64_t)sizeof(DltStorageHeader)-sizeof(pattern))));
        
        if (found_pattern){
            
            if (skipped_bytes)
                cerr << "skipped " << skipped_bytes << " bytes of data to find next storageheader pattern.\n";
            
            memcpy((char*)msg->storageheader, pattern, sizeof(pattern)); // we need to copy it as it will be used on export
            // read the rest of the dlt storage header:
            fin.read(((char*)msg->storageheader)+sizeof(pattern), sizeof(*msg->storageheader)-sizeof(pattern));
            remaining-= sizeof(*msg->storageheader)-sizeof(pattern);
            // now read dlt header:
            if (remaining >= static_cast<int64_t>(sizeof(*msg->standardheader))){
                fin.read((char*)msg->standardheader, sizeof(*msg->standardheader));
                remaining -= sizeof(*msg->standardheader);
                // verify header:
                // check version
                int header_version = (msg->standardheader->htyp & DLT_HTYP_VERS) >> 5; // need a constant for 5
                if ((header_version<DLT_HEADER_VERSION_MIN) || (header_version > DLT_HEADER_VERSION_MAX)){
                    cerr << "msg #" << nr_msgs << " has wrong header version (" << header_version << "). skipping! ";
                }else{
                    uint16_t len = DLT_BETOH_16(msg->standardheader->len); // why that? the macro should work! DLT_ENDIAN_GET_16(hstd.htyp, hstd.len); // len is without storage header (but with stdh)
                    if (len<=sizeof(*msg->standardheader)){
                        cerr << "msg len (" << len << ") <= sizeof(DltStandardHeader). skipping!\n";
                        // keep remaining untouched. pattern search will find next msg
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
                }
            }else{
                cerr << "no standard header after storage header found! Stop processing this file!\n";
                remaining=-2;
            }
        }else{
            cerr << "no proper DLT pattern found! Stop processing this file! Skipped " << skipped_bytes << " bytes\n";
            cerr << " found: <" << pattern[0] << pattern[1] << pattern[2] << pattern[3] << ">\n";
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
	uint32_t ecu_i;
	memcpy (&ecu_i, ecu, sizeof(ecu_i));
    LIST_OF_MSGS &list_of_msg = map_ecus[ecu_i].msgs;
    list_of_msg.push_back(msg);
    if (verbose>=3)
        debug_print_message(*msg);
    
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
        DltMessage *prev_msg = *it;
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
                    if (verbose>=2){
                        // show the msg that lead to a new lifecycle and the previous one.
                        if (prev_msg){
                            cout << "\nprev:";
                            debug_print_message(*prev_msg);
                        }
                        cout << "new :";
                        debug_print_message(**it);
                    }
                    Lifecycle new_lc(**it);
                    ecu.lcs.push_back(new_lc); // will be sorted later. so it doesn't matter where we add them
                    cur_l = ecu.lcs.end(); // get the one inserted
                    --cur_l; // end points to a non existing element.
                }
            } // else fits in cur_l -> next msg
            prev_msg = *it;
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

bool compare_usecbegin(const OverallLC &first, const OverallLC &second)
{
    if (first.usec_begin < second.usec_begin) return true;
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

int merge_lcs(ECU_Info &ecu)
{
    if (verbose>1) cout << "merging...\n";
    bool merged;
    do{
        merged=false;
        for (LIST_OF_LCS::iterator it = ecu.lcs.begin(); !merged && (it!=ecu.lcs.end()); ++it){
            LIST_OF_LCS::iterator j = it;
            j++;
            for(; !merged && (j!= ecu.lcs.end()); ++j){
                // check whether j overlaps with it:
                if ((*it).expand_if_intersects(*j)){
                    // now we need to delete j
                    assert((*j).msgs.size()==0);
                    ecu.lcs.erase(j);
                    j = ecu.lcs.begin(); // safer as otherwise it might be accessed after the deletion.
                    merged=true;
                }
            }
        }
    }while(merged);
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


OverallLC::OverallLC(const Lifecycle &lc)
{
    usec_begin=lc.usec_begin;
    usec_end=lc.usec_end;
    lcs.push_back(lc);
}

void OverallLC::debug_print() const
{
    time_t sbeg, send;
    sbeg = usec_begin / usecs_per_sec;
    send = usec_end / usecs_per_sec;
    
    cout << " LC from " << ctime(&sbeg) << "      to ";
    cout << ctime(&send);
    cout << "  num_lcs = " << lcs.size() << endl;
}


bool OverallLC::expand_if_intersects(const Lifecycle &lc)
{
    if (lc.usec_begin > usec_end) return false;
    if (lc.usec_end < usec_begin) return false;
    // ok, we intersect. do we need to expand?
    if (lc.usec_begin<usec_begin) {
        usec_begin = lc.usec_begin;
        lcs.push_front(lc);
    }else{
        lcs.push_back(lc);
    }
    if (lc.usec_end>usec_end) usec_end = lc.usec_end;
    
    return true;
}

bool OverallLC::output_to_fstream(std::ofstream &f, bool timeadjust)
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
     next_time in us resolution */
    VEC_OF_LC_it vec;
    
    /* then we always keep: index, next_index, next_time
     and output the msgs from vector[index] till >next_time
     then we determine new next_time and next index
     and keep on looping until next_index is empty (vector empty)
     */
    
    LC_it *index=NULL; // pointing to the LC_it with the min.time
    LC_it *next_index=NULL; // pointing to the LC_it with the min. time not from elem index
    int64_t next_time=0; // next time where the change from index to next_index has to happen
    
    int64_t min_time=0;
    for (LIST_OF_LCS::iterator it = lcs.begin(); it!=lcs.end(); ++it){
        LC_it l;
        l.it = (*it).msgs.begin();
        l.end = (*it).msgs.end();
        l.min_time= (*it).calc_min_time();
        l.usec_begin = (*it).usec_begin;
        l.clock_skew = (*it).clock_skew;
        vec.push_back(l);
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
            DltMessage *msg=*(index->it);
            int64_t tmsp = multiply((int64_t)(msg->headerextra.tmsp)*usecs_per_tmsp, index->clock_skew); // this needs to be done before output_message!
            if (timeadjust){
                // change the DltMessage
                int64_t t = index->usec_begin + multiply((((int64_t)msg->headerextra.tmsp) * usecs_per_tmsp),index->clock_skew);
                msg->storageheader->seconds = (uint32_t)(t / usecs_per_sec);
                msg->storageheader->microseconds = t % usecs_per_sec;
            }
            output_message(msg, f); // see below!
            ++(index->it);
            if (index->it != index->end){
                msg = *(index->it); // in case somebody uses it from now on.
                // update LC_it
                index->min_time -= tmsp;
                index->min_time += multiply(usecs_per_tmsp*((int64_t)(msg->headerextra.tmsp)), index->clock_skew);
            }else{
				msg = NULL; // in case somebody uses it
                // emptied this lc! we need to delete this element
                for (VEC_OF_LC_it::iterator j=vec.begin(); (NULL!=index) && (j!=vec.end()); ++j){
                    if (index==&(*j)){
                        vec.erase(j);
                        j = vec.begin(); // safer as otherwise it might be accessed after deletion!
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
        // output the msgs sequentially:
        LC_it &l=vec[0];
        for(; l.it!=l.end; ++l.it){
            DltMessage *msg = *(l.it);
            if (timeadjust){
                // change the DltMessage
                int64_t t = l.usec_begin + multiply(usecs_per_tmsp*((int64_t)msg->headerextra.tmsp), l.clock_skew);
                msg->storageheader->seconds = (uint32_t)(t / usecs_per_sec);
                msg->storageheader->microseconds = t % usecs_per_sec;
            }
            output_message(msg, f); // take care. after this call the message is corrupt! (endianess...)
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
    // now quickly sort them by start time:
    list_olcs.sort(compare_usecbegin);
    
    return 0; // success
}

std::string get_ofstream_name(int cnt, std::string const &templ)
{
    std::string name(templ);
    if (cnt>0){
        // do we have to remove the extension ".dlt" first?
        if (name.compare(name.length()-4, 4, ".dlt")==0){
            // remove the .extension. will be added later again
            name.erase(name.length()-4, 4);
        }
        // we have to add _xxx to the filename
        char nr[20];
        snprintf(nr, 19, "%03d", cnt); // todo as we know the max number upfront we could just use an many digits as needed
        name.append(nr);
        // but before the ".dlt"
        name.append(".dlt");
    }
    return name;
}

std::ofstream *get_ofstream(int cnt, std::string const &templ)
{
    std::string name(get_ofstream_name(cnt, templ));
    // now open the file:
    std::ofstream *f=new std::ofstream;
    f->open(name.c_str(), ios_base::out | ios_base::binary | ios_base::trunc);
    if (!(f->is_open())){
        delete f;
        f=NULL;
        cerr << "can't open <" << name << "> for writing! Aborting!";
        abort();
    }
    return f;
}

void debug_print_message(const DltMessage &msg)
{
    char ecu[5];
    ecu[4]=0;
    memcpy(ecu, msg.headerextra.ecu, 4);
    
    cout << "[" << ecu << "] " <<  msg.headerextra.tmsp;

    time_t sbeg;
    sbeg = msg.storageheader->seconds;
    char buf[100];
    strftime(buf, 99, "%Y/%m/%d %H:%M:%S", localtime(&sbeg));
    
    cout << " " << buf << "." << setw(6) << msg.storageheader->microseconds <<
        " " << (int)msg.standardheader->htyp <<
        " " << (int)msg.standardheader->mcnt <<
        " " << DLT_BETOH_16(msg.standardheader->len) << endl;
}

int64_t multiply(int64_t a, double b)
{
    // we do a special multiplication here to avoid rounding errors
    // we multiply assuming that the upper 15bits are not used (as this would
    // be really huge times presented even in usecs.
    // todo we could further optimize as we know that 0.5 <= b <= 1.5
    const int shift_bits=15;
    assert((a >> (63-shift_bits))==0);
    
    unsigned int m = static_cast<unsigned int>((1u<<shift_bits) * b);
    a *=m;
    a >>= shift_bits;
    return a;
}
