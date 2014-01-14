//
//  main.cpp
//  dlt-sort
//
//  Created by Matthias Behr on 31.12.13.
//  Copyright (c) 2013, 2014 Matthias Behr. All rights reserved.
//

#include <getopt.h>

#include "dlt-sort.h"

using namespace std;

const char* const dlt_sort_version="1.0";

void print_usage()
{
    cout << "usage dlt-sort [options] input-file input-file ...\n";
    cout << " -s --split    split output file automatically one for each lifecycle\n";
    cout << " -f --file outputfilename (default dlt_sorted.dlt). If split is active xxx.dlt will be added automatically.\n";
    cout << " -t --timestamps adjust time in storageheader to detected lifecycle time. Changes the orig. logs!\n";
    cout << " -h --help     show usage/help\n";
    cout << " -v --verbose  set verbose level to 1 (increase by adding more -v)\n";
}

int main(int argc, char * argv[])
{
    cout << "dlt-sort (v" << dlt_sort_version << ") (c) 2013, 2014 Matthias Behr\n";
    
    if (argc==1){
        print_usage();
        return -1;
    }

    int c, option_index;
    bool do_split=false; // by default don't split output files per lifecycle
    bool do_timeadjust=false; // by default don't adjust timestamps in generated dlt file
    std::string ofilename ("dlt_sorted.dlt");
    
    static struct option long_options[] =
    {
        /* These options set a flag. */
        {"verbose", no_argument,       &verbose, 1},
        // {"brief",   no_argument,       &verbose_flag, 0},
        /* These options don't set a flag.
         We distinguish them by their indices. */
        {"split",     no_argument,       0, 's'},
        {"timestamps", no_argument, 0, 't'},
        {"file",    required_argument, 0, 'f'},
        {"help", no_argument, 0, 'h'},
        {0, 0, 0, 0}
    };
    while ((c = getopt_long (argc, argv, "vhstf:", long_options, &option_index))!= -1){
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
                do_split=true;
                if(verbose) cout << " splitting output files by lifecycles\n";
                break;
                case 't':
                do_timeadjust = true;
                cout <<" adjusting timestamps. This changes the orig. logs!\n";
                break;
            case 'f':
                ofilename=std::string (optarg);
                if(verbose) cout << " using <" << ofilename << "> as output file name\n";
                break;
            default:
                abort();
        }
    }
    argc-=optind;
    argv+=optind;
    
	// some assertions:
	assert(sizeof(DltStorageHeader) == 16);
	assert(sizeof(DltStandardHeader) == 4);
	assert(sizeof(DltStandardHeaderExtra) == 12);
	assert(sizeof(DltExtendedHeader) == 10);

    // print some verbose infos:
    if (verbose){
        cout << " set verbose level to " << verbose << endl;
    }
    
    // let's process the input files:
    for (option_index=0; option_index<argc; option_index++){
        printf("Processing file %s:\n", argv[option_index]);
        std::ifstream fin;
        fin.open(argv[option_index], ios::in|ios::binary);
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
        
        char ecu[5];
        ecu[4]=0;
        memcpy(ecu, (char*) &it->first, sizeof(uint32_t));
        size_t nr_lcs = info.lcs.size();
        cout << "ECU <" << ecu << "> contains " << nr_lcs << " lifecycle\n";
        debug_print(info.lcs);
        
        // now see whether they overall (the detection does not always work 100%
        // esp. on short lifecycles:
        merge_lcs(info);
        sort_msgs_lcs(info);
        if (info.lcs.size() != nr_lcs){
            cout << "ECU <" << ecu << "> contains " << info.lcs.size() << " lifecycle after merge:\n";
            debug_print(info.lcs);
        }
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
        (*it).output_to_fstream(*f, do_timeadjust); // todo error handling
        ++f_cnt;
    }
    if (f) f->close();
    
    // free memory: (not really needed as we exit anyhow here but to make valgrind,... happy:
    for (MAP_OF_ECUS::iterator it = map_ecus.begin(); it != map_ecus.end(); ++it){
       if (verbose>=2) cout << "deallocating " << it->second.msgs.size() << " msgs\n";
        for (LIST_OF_MSGS::iterator j = it->second.msgs.begin(); j != it->second.msgs.end(); ++j){
            DltMessage *m = *j;
            if (m->databuffer) delete m->databuffer;
            delete m;
        }
    }

    
    return 0; // no error (<0 for error)
}

