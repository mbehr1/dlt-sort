//
//  main.cpp
//  dlt-sort
//
//  Created by Matthias Behr on 31.12.13.
//  Copyright (c) 2013 Matthias Behr. All rights reserved.
//

#include <iostream>
#include <fstream>

#include <getopt.h>
#include <dlt/dlt_common.h>

using namespace std;

int verbose = 0;

void print_usage()
{
    cout << "usage dlt-sort [options] input-file input-file ...\n";
    cout << " -s --split    split output file automatically one for each lifecycle\n";
    cout << " -h --help     show usage/help\n";
    cout << " -v --verbose  set verbose level to 1 (increase by adding more -v)\n";
}

int process_input(std::ifstream &);

int main(int argc, char * argv[])
{
    cout << "dlt-sort (c) 2013 Matthias Behr\n";
    
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
        // check for dlt storage header:
        DltStorageHeader sh;
        fin.read((char*)&sh, sizeof(sh));
        remaining-= sizeof(sh);
        if (0 == memcmp(sh.pattern, DLT_ID4_ID, sizeof(sh.pattern))){
            // now read dlt header:
            DltStandardHeader hstd;
            if (remaining >= sizeof(hstd)){
                fin.read((char*)&hstd, sizeof(hstd));
                remaining -= sizeof(hstd);
                // verify header (somehow)
                uint16_t len = DLT_BETOH_16(hstd.len); // fixme: why that? the macro should work! DLT_ENDIAN_GET_16(hstd.htyp, hstd.len); // len is without storage header (but with stdh)
                if (len<=sizeof(hstd)){
                    cerr << "msg len <= sizeof(DltStandardHeader). Stopping processing this file!\n";
                    remaining = -4;
                }else{
                    len -= sizeof(hstd); // standard header already read from this message
                    // read remaining message:
                    if (remaining >= len){
                        char *msgdata = new char [len];
                        fin.read(msgdata, len);
                        delete msgdata;
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
            cerr << " found: <" << sh.pattern[0] << sh.pattern[1] << sh.pattern[2] << sh.pattern[3] << ">\n";
            remaining = -1;
        }
        
    }
    if (verbose && remaining!=0) cout << "remaining != 0. parsing errors within that file!\n";
    if (verbose) cout << "processed " << nr_msgs << " msgs\n";
    return (int)remaining; // 0 = success, <0 error in processing
}
