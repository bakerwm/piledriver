// ***************************************************************************
// bamtools_convert.cpp (c) 2010 Derek Barnett, Erik Garrison
// Marth Lab, Department of Biology, Boston College
// ---------------------------------------------------------------------------
// Last modified: 10 December 2012
// ---------------------------------------------------------------------------
// Converts between BAM and a number of other formats
// ***************************************************************************

#include "piledriver.h"

#include <api/BamConstants.h>
#include <api/BamMultiReader.h>
#include <utils/bamtools_fasta.h>
#include <utils/bamtools_options.h>
#include <utils/bamtools_pileup_engine.h>
#include <utils/bamtools_utilities.h>
using namespace BamTools;

#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>
using namespace std;
  
namespace BamTools { 
  
// ---------------------------------------------
// ConvertTool constants

// supported conversion format command-line names
static const string FORMAT_PILEUP = "pileup";


// other constants
static const unsigned int FASTA_LINE_MAX = 50;

// ---------------------------------------------
// ConvertPileupFormatVisitor declaration

class PileDriverPileupFormatVisitor : public PileupVisitor {

    // ctor & dtor
    public:        
        PileDriverPileupFormatVisitor(const RefVector& references,
                                      const string& fastaFilename,
                                      ostream* out,
                                      int num_samples, 
                                      map<string, size_t> &sample_map);
        
        ~PileDriverPileupFormatVisitor(void);

    // PileupVisitor interface implementation
    public:
        void Header();
        void Visit(const PileupPosition& pileupData);

    // data members
    private:
        Fasta     m_fasta;
        bool      m_hasFasta;
        ostream*  m_out;
        int       m_num_samples;
        map<string, size_t> m_sample_map;
        RefVector m_references;
};
    
} // namespace BamTools
  
// ---------------------------------------------
// ConvertSettings implementation

struct PileDriverTool::PileDriverSettings {

    // flag
    bool HasInput;
    bool HasInputFilelist;
    bool HasOutput;
    bool HasFormat;
    bool HasRegion;
    bool HasMinMAPQ;

    // pileup flags
    bool HasFastaFilename;
    bool IsOmittingSamHeader;
    bool IsPrintingPileupMapQualities;
    
    // options
    vector<string> InputFiles;
    string InputFilelist;
    string OutputFilename;
    string Format;
    string Region;
    size_t minMAPQ;
    
    // pileup options
    string FastaFilename;

    // constructor
    PileDriverSettings(void)
        : HasInput(false)
        , HasInputFilelist(false)
        , HasOutput(false)
        , HasFormat(false)
        , HasRegion(false)
        , HasMinMAPQ(false)
        , HasFastaFilename(false)
        , IsOmittingSamHeader(false)
        , IsPrintingPileupMapQualities(false)
        , OutputFilename(Options::StandardOut())
        , FastaFilename("")
    { } 
};    

// ---------------------------------------------
// ConvertToolPrivate implementation  
  
struct PileDriverTool::PileDriverToolPrivate {
  
    // ctor & dtor
    public:
        PileDriverToolPrivate(PileDriverTool::PileDriverSettings* settings)
            : m_settings(settings)
            , m_out(cout.rdbuf())
        { }

        ~PileDriverToolPrivate(void) { }
    
    // interface
    public:
        bool Run(void);
        
    // internal methods
    private:
        // special case - uses the PileupEngine
        bool RunPileupConversion(BamMultiReader* reader);
        
    // data members
    private: 
        PileDriverTool::PileDriverSettings* m_settings;
        RefVector m_references;
        ostream m_out;
};

bool PileDriverTool::PileDriverToolPrivate::Run(void) {
 
    // ------------------------------------
    // initialize conversion input/output
    // set to default input if none provided
    if ( !m_settings->HasInput && !m_settings->HasInputFilelist )
        m_settings->InputFiles.push_back(Options::StandardIn());
    
    // add files in the filelist to the input file list
    if ( m_settings->HasInputFilelist ) {

        ifstream filelist(m_settings->InputFilelist.c_str(), ios::in);
        if ( !filelist.is_open() ) {
            cerr << "bamtools convert ERROR: could not open input BAM file list... Aborting." << endl;
            return false;
        }

        string line;
        while ( getline(filelist, line) )
            m_settings->InputFiles.push_back(line);
    }

    // open input files
    BamMultiReader reader;
    if ( !reader.Open(m_settings->InputFiles) ) {
        cerr << "bamtools convert ERROR: could not open input BAM file(s)... Aborting." << endl;
        return false;
    }

    // if input is not stdin & a region is provided, look for index files
    if ( m_settings->HasInput && m_settings->HasRegion ) {
        if ( !reader.LocateIndexes() ) {
            cerr << "bamtools convert ERROR: could not locate index file(s)... Aborting." << endl;
            return false;
        }
    }

    // retrieve reference data
    m_references = reader.GetReferenceData();

    // set region if specified
    BamRegion region;
    if ( m_settings->HasRegion ) 
    {
        if ( Utilities::ParseRegionString(m_settings->Region, reader, region) ) 
        {

            if ( reader.HasIndexes() ) {
                if ( !reader.SetRegion(region) ) {
                    cerr << "bamtools convert ERROR: set region failed. Check that REGION describes a valid range" << endl;
                    reader.Close();
                    return false;
                }
            }

        } else {
            cerr << "bamtools convert ERROR: could not parse REGION: " << m_settings->Region << endl;
            cerr << "Check that REGION is in valid format (see documentation) and that the coordinates are valid"
                 << endl;
            reader.Close();
            return false;
        }
    }
        
    // if output file given
    ofstream outFile;
    if ( m_settings->HasOutput ) {
      
        // open output file stream
        outFile.open(m_settings->OutputFilename.c_str());
        if ( !outFile ) {
            cerr << "bamtools convert ERROR: could not open " << m_settings->OutputFilename
                 << " for output" << endl;
            return false; 
        }
        
        // set m_out to file's streambuf
        m_out.rdbuf(outFile.rdbuf()); 
    }
    
    // -------------------------------------
    // do conversion based on format
    
    bool convertedOk = true;
    
    convertedOk = RunPileupConversion(&reader);
    
    // ------------------------
    // clean up & exit
    reader.Close();
    if ( m_settings->HasOutput )
        outFile.close();
    return convertedOk;   
}



bool PileDriverTool::PileDriverToolPrivate::RunPileupConversion(BamMultiReader* reader) {
  
    // check for valid BamMultiReader
    if ( reader == 0 ) return false;

    // set up our output 'visitor'
    map<string, size_t> sample_map;
    for (size_t i = 0; i < m_settings->InputFiles.size(); ++i)
        sample_map[m_settings->InputFiles[i]] = i;

    PileDriverPileupFormatVisitor* cv = 
        new PileDriverPileupFormatVisitor(m_references,
                              m_settings->FastaFilename,
                              &m_out,
                              m_settings->InputFiles.size(),
                              sample_map);
    // set up PileupEngine
    PileupEngine pileup;
    pileup.AddVisitor(cv);
    
    // print a header
    cv->Header();
    
    // iterate through data
    BamAlignment al;
    while ( reader->GetNextAlignment(al) )
        pileup.AddAlignment(al);
    pileup.Flush();
    
    // clean up
    delete cv;
    cv = 0;
    
    // return success
    return true;
}       

// ---------------------------------------------
// ConvertTool implementation

PileDriverTool::PileDriverTool(void)
    : AbstractTool()
    , m_settings(new PileDriverSettings)
    , m_impl(0)
{
    // set program details
    Options::SetProgramInfo("bamtools piledriver", 
                            "converts BAM to a number of other formats",
                            "-format <FORMAT> [-in <filename> -in <filename> ... | -list <filelist>] [-out <filename>] [-region <REGION>] [format-specific options]");
    
    // set up options 
    OptionGroup* IO_Opts = Options::CreateOptionGroup("Input & Output");
    
    Options::AddValueOption("-in", "BAM filename", 
                            "the input BAM file(s)", "", 
                            m_settings->HasInput,   
                            m_settings->InputFiles,     
                            IO_Opts, 
                            Options::StandardIn());

    Options::AddValueOption("-list", "filename", 
                            "the input BAM file list, one line per file", "", 
                            m_settings->HasInputFilelist,  
                            m_settings->InputFilelist, 
                            IO_Opts);

    Options::AddValueOption("-out", "BAM filename", "the output BAM file",  "", 
                            m_settings->HasOutput,  
                            m_settings->OutputFilename, 
                            IO_Opts, 
                            Options::StandardOut());
    
    Options::AddValueOption("-region", "REGION", 
                            "genomic region. Index file is recommended for better performance, and is used automatically if it exists. Regions specified using the following format: -region chr1:START..END", "", 
                            m_settings->HasRegion, 
                            m_settings->Region, 
                            IO_Opts);

    OptionGroup* PileupOpts = Options::CreateOptionGroup("Pileup Options");
    
    Options::AddValueOption("-fasta", "FASTA filename", 
                            "FASTA reference file", "", 
                            m_settings->HasFastaFilename, 
                            m_settings->FastaFilename, 
                            PileupOpts);
}

PileDriverTool::~PileDriverTool(void) {

    delete m_settings;
    m_settings = 0;
    
    delete m_impl;
    m_impl = 0;
}

int PileDriverTool::Help(void) {
    Options::DisplayHelp();
    return 0;
}

int PileDriverTool::Run(int argc, char* argv[]) {
  
    // parse command line arguments
    Options::Parse(argc, argv, 1);
    
    // initialize ConvertTool with settings
    m_impl = new PileDriverToolPrivate(m_settings);
    
    // run ConvertTool, return success/fail
    if ( m_impl->Run() ) 
        return 0;
    else 
        return 1;
}



// ---------------------------------------------
// ConvertPileupFormatVisitor implementation

PileDriverPileupFormatVisitor::PileDriverPileupFormatVisitor(
    const RefVector& references, 
    const string& fastaFilename,
    ostream* out,
    int num_samples,
    map<string, size_t> &sample_map
)
    : PileupVisitor()
    , m_hasFasta(false)
    , m_out(out)
    , m_num_samples(num_samples)
    , m_sample_map(sample_map)
    , m_references(references)
{   
    // set up Fasta reader if file is provided
    if ( !fastaFilename.empty() ) {
      
        // check for FASTA index
        string indexFilename = "";
        if ( Utilities::FileExists(fastaFilename + ".fai") ) 
            indexFilename = fastaFilename + ".fai";
      
        // open FASTA file
        if ( m_fasta.Open(fastaFilename, indexFilename) ) 
            m_hasFasta = true;
    }
}

PileDriverPileupFormatVisitor::~PileDriverPileupFormatVisitor(void) { 
    // be sure to close Fasta reader
    if ( m_hasFasta ) {
        m_fasta.Close();
        m_hasFasta = false;
    }
}

void PileDriverPileupFormatVisitor::Header() {
    *m_out << "chrom\t"
           << "start\t"
           << "end\t"
           << "ref\t"
           << "depth\t"
           << "r_depth\t"
           << "a_depth\t"
           << "num_A\t"
           << "num_C\t"
           << "num_G\t"
           << "num_T\t"
           << "num_D\t"
           << "num_I\t"
           << "totQ_A\t"
           << "totQ_C\t"
           << "totQ_G\t"
           << "totQ_T\t"
           << "all_ins\t"
           // forward strand
           << "num_F_A\t"
           << "num_F_C\t"
           << "num_F_G\t"
           << "num_F_T\t"
           << "num_F_D\t"
           << "num_F_I\t"
           << "totQ_F_A\t"
           << "totQ_F_C\t"
           << "totQ_F_G\t"
           << "totQ_F_T\t"
           << "all_F_ins\t"
           // reverse strand
           << "num_R_A\t"
           << "num_R_C\t"
           << "num_R_G\t"
           << "num_R_T\t"
           << "num_R_D\t"
           << "num_R_I\t"
           << "totQ_R_A\t"
           << "totQ_R_C\t"
           << "totQ_R_G\t"
           << "totQ_R_T\t"
           << "all_R_ins";
    
    for (size_t i = 0; i < m_num_samples; ++i)
    {
        *m_out 
            << "\tsample_" << i + 1;
    }
     *m_out << endl;
}


void PileDriverPileupFormatVisitor::Visit(const PileupPosition& pileupData ) {
  
    // skip if no alignments at this position
    if ( pileupData.PileupAlignments.empty() ) return;
  
    // retrieve reference name
    const string& referenceName = m_references[pileupData.RefId].RefName;
    const int& position   = pileupData.Position;
    
    // retrieve reference base from FASTA file, if one provided;
    // otherwise default to 'N'
    char referenceBase('N');
    if ( m_hasFasta && (pileupData.Position < 
        m_references[pileupData.RefId].RefLength) ) 
    {
        if ( !m_fasta.GetBase(pileupData.RefId, 
                              pileupData.Position, 
                              referenceBase ) ) 
        {
            cerr << "bamtools convert ERROR: "
                 << "pileup conversion - "
                 << "could not read reference base from FASTA file" 
                 << endl;
            return;
        }
    }
    
    // get count of alleles at this position
    const uint64_t total_depth = pileupData.PileupAlignments.size();
    uint64_t total_ref_depth = 0;
    uint64_t total_alt_depth = 0;
    
    // coverage info for each sample
    vector<SampleCoverage> sample_cov(m_num_samples + 1);
    
    // iterate over alignments at this pileup position
    vector<PileupAlignment>::const_iterator pileupIter = \
         pileupData.PileupAlignments.begin();
    vector<PileupAlignment>::const_iterator pileupEnd  = \
         pileupData.PileupAlignments.end();
    for ( ; pileupIter != pileupEnd; ++pileupIter ) {
        
        const PileupAlignment pa = (*pileupIter);
        const BamAlignment& ba = pa.Alignment;
        const size_t file_id = m_sample_map[ba.Filename];
        const size_t ovrl_idx = sample_cov.size() - 1;
        // if current base is not a DELETION
        if ( !pa.IsCurrentDeletion ) {
            
            // get base at current position
            char base = ba.QueryBases.at(pa.PositionInAlignment);
            
            if (toupper(base) == toupper(referenceBase))
                total_ref_depth++;
            else
                total_alt_depth++;

            short qual = \
              static_cast<short>(ba.Qualities.at(pa.PositionInAlignment)) - 33;
            
            // if next position contains insertion
            if ( pa.IsNextInsertion ) {
                
                stringstream ins_bases("");
                
                for (int i = 1; i <= pa.InsertionLength; ++i) {
                    char insertedBase = 
                        (char)ba.QueryBases.at(pa.PositionInAlignment + i);
                    ins_bases << insertedBase;
                }
                
                if (! ba.IsReverseStrand()) 
                {
                    sample_cov[file_id].ins_fwd_cnt++;
                    sample_cov[file_id].ins_fwd_alleles.push_back(ins_bases.str());
                    sample_cov[ovrl_idx].ins_fwd_cnt++;
                    sample_cov[ovrl_idx].ins_fwd_alleles.push_back(ins_bases.str());
                }
                else {
                    sample_cov[file_id].ins_rev_cnt++;
                    sample_cov[file_id].ins_rev_alleles.push_back(ins_bases.str());
                    sample_cov[ovrl_idx].ins_rev_cnt++;
                    sample_cov[ovrl_idx].ins_rev_alleles.push_back(ins_bases.str());
                }
            }
            
            if ((base == 'A') || (base == 'a'))
            {
                if (! ba.IsReverseStrand()) 
                {
                    sample_cov[file_id].a_fwd_cnt++;
                    sample_cov[file_id].a_fwd_totqual += qual;
                    sample_cov[ovrl_idx].a_fwd_cnt++;
                    sample_cov[ovrl_idx].a_fwd_totqual += qual;
                }
                else 
                {
                    sample_cov[file_id].a_rev_cnt++;
                    sample_cov[file_id].a_rev_totqual += qual;
                    sample_cov[ovrl_idx].a_rev_cnt++;
                    sample_cov[ovrl_idx].a_rev_totqual += qual;          
                }
            }
            else if ((base == 'C') || (base == 'c'))
            {
                if (! ba.IsReverseStrand()) 
                {
                    sample_cov[file_id].c_fwd_cnt++;
                    sample_cov[file_id].c_fwd_totqual += qual;
                    sample_cov[ovrl_idx].c_fwd_cnt++;
                    sample_cov[ovrl_idx].c_fwd_totqual += qual;
                }
                else 
                {
                    sample_cov[file_id].c_rev_cnt++;
                    sample_cov[file_id].c_rev_totqual += qual;
                    sample_cov[ovrl_idx].c_rev_cnt++;
                    sample_cov[ovrl_idx].c_rev_totqual += qual;          
                }

            }
            else if ((base == 'G') || (base == 'g'))
            {
                if (! ba.IsReverseStrand()) 
                {
                    sample_cov[file_id].g_fwd_cnt++;
                    sample_cov[file_id].g_fwd_totqual += qual;
                    sample_cov[ovrl_idx].g_fwd_cnt++;
                    sample_cov[ovrl_idx].g_fwd_totqual += qual;
                }
                else 
                {
                    sample_cov[file_id].g_rev_cnt++;
                    sample_cov[file_id].g_rev_totqual += qual;
                    sample_cov[ovrl_idx].g_rev_cnt++;
                    sample_cov[ovrl_idx].g_rev_totqual += qual;          
                }

            }
            else if ((base == 'T') || (base == 't'))
            {
                if (! ba.IsReverseStrand()) 
                {
                    sample_cov[file_id].t_fwd_cnt++;
                    sample_cov[file_id].t_fwd_totqual += qual;
                    sample_cov[ovrl_idx].t_fwd_cnt++;
                    sample_cov[ovrl_idx].t_fwd_totqual += qual;
                }
                else 
                {
                    sample_cov[file_id].t_rev_cnt++;
                    sample_cov[file_id].t_rev_totqual += qual;
                    sample_cov[ovrl_idx].t_rev_cnt++;
                    sample_cov[ovrl_idx].t_rev_totqual += qual;          
                }
            }
        }
        // it is a deletion
        else {
            if (! ba.IsReverseStrand()) 
            {
                sample_cov[file_id].del_fwd_cnt++;
                sample_cov[ovrl_idx].del_fwd_cnt++;
            }
            else 
            {
                sample_cov[file_id].del_rev_cnt++;
                sample_cov[ovrl_idx].del_rev_cnt++;
            }

            total_alt_depth++;
        }
    }
    
    // ----------------------
    // print results 
    
    stringstream all_ins_fwd_alleles("");
    stringstream all_ins_rev_alleles("");
    stringstream all_ins_alleles;
    if (sample_cov[m_num_samples].ins_fwd_alleles.size() > 0) 
    {
        
        for(size_t s = 0; s < sample_cov[m_num_samples].ins_fwd_alleles.size(); ++s)
        {
            if(s != 0)
                all_ins_fwd_alleles << ",";
            all_ins_fwd_alleles << sample_cov[m_num_samples].ins_fwd_alleles[s];
        }
    }
    else {
        all_ins_fwd_alleles << ".";
    }
    
    if (sample_cov[m_num_samples].ins_rev_alleles.size() > 0) 
    {
        
        for(size_t s = 0; s < sample_cov[m_num_samples].ins_rev_alleles.size(); ++s)
        {
            if(s != 0)
                all_ins_rev_alleles << ",";
            all_ins_rev_alleles << sample_cov[m_num_samples].ins_rev_alleles[s];
        }
    }
    else {
        all_ins_rev_alleles << ".";
    }
    
    if ((sample_cov[m_num_samples].ins_rev_alleles.size() > 0) 
        ||
        (sample_cov[m_num_samples].ins_rev_alleles.size() > 0))
    {
        all_ins_alleles << all_ins_fwd_alleles.str() << all_ins_rev_alleles.str();
    }
    else {
        all_ins_alleles << ".";
    }

    size_t all_idx = m_num_samples;
    
    const string TAB = "\t";
    *m_out << referenceName       << TAB 
           << position            << TAB 
           << position + 1        << TAB 
           << referenceBase       << TAB 
           << total_depth         << TAB
           << total_ref_depth     << TAB
           << total_alt_depth     << TAB

           // overall allele counts
           << sample_cov[all_idx].a_fwd_cnt + sample_cov[all_idx].a_rev_cnt << TAB
           << sample_cov[all_idx].c_fwd_cnt + sample_cov[all_idx].c_rev_cnt << TAB
           << sample_cov[all_idx].g_fwd_cnt + sample_cov[all_idx].g_rev_cnt << TAB
           << sample_cov[all_idx].t_fwd_cnt + sample_cov[all_idx].t_rev_cnt << TAB
           << sample_cov[all_idx].del_fwd_cnt + sample_cov[all_idx].del_rev_cnt << TAB
           << sample_cov[all_idx].ins_fwd_cnt + sample_cov[all_idx].ins_rev_cnt << TAB

           // overall allele qualities (and insertion alleles)
           << sample_cov[all_idx].a_fwd_totqual + sample_cov[all_idx].a_rev_totqual << TAB
           << sample_cov[all_idx].c_fwd_totqual + sample_cov[all_idx].c_rev_totqual << TAB
           << sample_cov[all_idx].g_fwd_totqual + sample_cov[all_idx].g_rev_totqual << TAB
           << sample_cov[all_idx].t_fwd_totqual + sample_cov[all_idx].t_rev_totqual << TAB
           << all_ins_alleles.str() << TAB
    
           // overall forward allele counts
           << sample_cov[all_idx].a_fwd_cnt << TAB
           << sample_cov[all_idx].c_fwd_cnt << TAB
           << sample_cov[all_idx].g_fwd_cnt << TAB
           << sample_cov[all_idx].t_fwd_cnt << TAB
           << sample_cov[all_idx].del_fwd_cnt  << TAB
           << sample_cov[all_idx].ins_fwd_cnt  << TAB

           // overall forward allele total qualities (and insertion alleles)
           << sample_cov[all_idx].a_fwd_totqual << TAB
           << sample_cov[all_idx].c_fwd_totqual << TAB
           << sample_cov[all_idx].g_fwd_totqual << TAB
           << sample_cov[all_idx].t_fwd_totqual << TAB
           << all_ins_fwd_alleles.str() << TAB

           // overall reverse allele counts
           << sample_cov[all_idx].a_rev_cnt << TAB
           << sample_cov[all_idx].c_rev_cnt << TAB
           << sample_cov[all_idx].g_rev_cnt << TAB
           << sample_cov[all_idx].t_rev_cnt << TAB
           << sample_cov[all_idx].del_rev_cnt  << TAB
           << sample_cov[all_idx].ins_rev_cnt  << TAB

           // overall reverse allele total qualities (and insertion alleles)
           << sample_cov[all_idx].a_rev_totqual << TAB
           << sample_cov[all_idx].c_rev_totqual << TAB
           << sample_cov[all_idx].g_rev_totqual << TAB
           << sample_cov[all_idx].t_rev_totqual << TAB
           << all_ins_rev_alleles.str();
           

    for (size_t i = 0; i < m_num_samples; ++i)
    {        
        stringstream sample_ins_fwd_alleles, sample_ins_rev_alleles;
        
        if (sample_cov[i].ins_fwd_alleles.size() > 0) {
        
            for(size_t s = 0; s < sample_cov[i].ins_fwd_alleles.size(); ++s)
            {
                if(s != 0)
                    sample_ins_fwd_alleles << ",";
                sample_ins_fwd_alleles << sample_cov[i].ins_fwd_alleles[s];
            }
        }
        else {
            sample_ins_fwd_alleles << ".";
        }
        
        if (sample_cov[i].ins_rev_alleles.size() > 0) {
        
            for(size_t s = 0; s < sample_cov[i].ins_rev_alleles.size(); ++s)
            {
                if(s != 0)
                    sample_ins_rev_alleles << ",";
                sample_ins_rev_alleles << sample_cov[i].ins_rev_alleles[s];
            }
        }
        else {
            sample_ins_rev_alleles << ".";
        }
        
        *m_out 
          << TAB
          // num_fwd(A)|totqual_fwd(A)|num_rev(A)|totqual_rev(A)
          << sample_cov[i].a_fwd_cnt << "|" << sample_cov[i].a_fwd_totqual << "|"
          << sample_cov[i].a_rev_cnt << "|" << sample_cov[i].a_rev_totqual 
          << ","
          // num_fwd(C)|totqual_fwd(C)|num_rev(C)|totqual_rev(C)
          << sample_cov[i].c_fwd_cnt << "|" << sample_cov[i].c_fwd_totqual << "|"
          << sample_cov[i].c_rev_cnt << "|" << sample_cov[i].c_rev_totqual
          << ","
          // num_fwd(G)|totqual_fwd(G)|num_rev(G)|totqual_rev(G)
          << sample_cov[i].g_fwd_cnt << "|" << sample_cov[i].g_fwd_totqual << "|"
          << sample_cov[i].g_rev_cnt << "|" << sample_cov[i].g_rev_totqual
          << ","
          // num_fwd(T)|totqual_fwd(T)|num_rev(T)|totqual_rev(T)
          << sample_cov[i].t_fwd_cnt << "|" << sample_cov[i].t_fwd_totqual << "|"
          << sample_cov[i].t_rev_cnt << "|" << sample_cov[i].t_rev_totqual
          << ","
          // num_fwd(D)|totqual_fwd(D)|num_rev(D)|totqual_rev(D)
          << sample_cov[i].del_fwd_cnt << "|." << sample_cov[i].del_rev_cnt << "|."
          << ","
          // ins_fwd|ins_rev
          << sample_ins_fwd_alleles.str() << "|" << sample_ins_rev_alleles.str();
    }
    *m_out << endl;
}
