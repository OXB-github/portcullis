//  ********************************************************************
//  This file is part of Portculis.
//
//  Portculis is free software: you can redistribute it and/or modify
//  it under the terms of the GNU General Public License as published by
//  the Free Software Foundation, either version 3 of the License, or
//  (at your option) any later version.
//
//  Portculis is distributed in the hope that it will be useful,
//  but WITHOUT ANY WARRANTY; without even the implied warranty of
//  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//  GNU General Public License for more details.
//
//  You should have received a copy of the GNU General Public License
//  along with Portculis.  If not, see <http://www.gnu.org/licenses/>.
//  *******************************************************************

#pragma once

#include "bam_utils.hpp"
#include "intron.hpp"
#include "junction.hpp"
#include "genome_mapper.hpp"
#include "depth_parser.hpp"
using portculis::bamtools::BamUtils;
using portculis::DepthParser;
using portculis::Intron;
using portculis::IntronHasher;
using portculis::Junction;
using portculis::JunctionPtr;

#include <boost/algorithm/string/split.hpp>
#include <boost/exception/all.hpp>
#include <boost/timer/timer.hpp>
using boost::split;
using boost::lexical_cast;
using boost::timer::auto_cpu_timer;

#include <fstream>
#include <vector>
#include <memory>
#include <unordered_map>
using std::ofstream;
using std::shared_ptr;

typedef std::unordered_map<Intron, JunctionPtr, IntronHasher> DistinctJunctions;
typedef std::unordered_map<Intron, JunctionPtr, IntronHasher>::iterator JunctionMapIterator;
typedef std::pair<const Intron, JunctionPtr> JunctionMapType;

namespace portculis {
class JunctionSystem {

private:    
    DistinctJunctions distinctJunctions;
    JunctionList junctionList;
    
    int32_t minQueryLength;
    double meanQueryLength;
    int32_t maxQueryLength;
    
    RefVector refs;
    SplicedAlignmentMap splicedAlignmentMap;
    
    
    size_t createJunctionGroup(size_t index, vector<JunctionPtr>& group) {
        
        JunctionPtr junc = junctionList[index];        
        group.push_back(junc);
        
        bool foundMore = false;
        for(size_t j = index + 1; j < junctionList.size(); j++) {

            JunctionPtr next = junctionList[j];

            if (junc->sharesDonorOrAcceptor(next)) {
                group.push_back(next);                    
                junc = next;                    
            }
            else {
                return j-1;                
            }
        }
        
        return foundMore ? index : junctionList.size()-1;
    }
   
    void findJunctions(const int32_t refId, JunctionList& subset) {
        
        subset.clear();
        for(JunctionPtr j : junctionList) {
            if (j->getIntron()->refId == refId) {
                subset.push_back(j);
            }    
        }        
    }
    
    
public:
    
    
    JunctionSystem() {
        minQueryLength = 0;
        meanQueryLength = 0.0;
        maxQueryLength = 0;         
    }
    
    virtual ~JunctionSystem() {
             
    }
    
    
    size_t size() {
        return distinctJunctions.size();
    }
    
    double getMeanQueryLength() const {
        return meanQueryLength;
    }

    void setMeanQueryLength(double meanQueryLength) {
        this->meanQueryLength = meanQueryLength;
    }
    
    int32_t getMaxQueryLength() const {
        return maxQueryLength;
    }

    void setMaxQueryLength(int32_t maxQueryLength) {
        this->maxQueryLength = maxQueryLength;
    }

    int32_t getMinQueryLength() const {
        return minQueryLength;
    }

    void setMinQueryLength(int32_t minQueryLength) {
        this->minQueryLength = minQueryLength;
    }

    void setQueryLengthStats(int32_t min, double mean, int32_t max) {
        this->minQueryLength = min;
        this->meanQueryLength = mean;
        this->maxQueryLength = max;
    }
    
    void setRefs(RefVector refs) {
        this->refs = refs;
    }
    
    /**
     * Adds any new junctions found from the given alignment to the set managed 
     * by this class
     * @param al The alignment to search for junctions
     * @return Whether a junction was found in this alignment or not
     */
    bool addJunctions(const BamAlignment& al, bool strandSpecific) {
        return addJunctions(al, 0, al.Position, strandSpecific);
    }
    
    bool addJunctions(const BamAlignment& al, const size_t startOp, const int32_t offset, bool strandSpecific) {
        
        bool foundJunction = false;
        
        size_t nbOps = al.CigarData.size();
        
        int32_t refId = al.RefID;
        int32_t lStart = offset;        
        int32_t lEnd = lStart;
        int32_t rStart = lStart;
        int32_t rEnd = lStart;
        
        for(size_t i = startOp; i < nbOps; i++) {
            
            CigarOp op = al.CigarData[i];
            if (op.Type == 'N') {
                foundJunction = true;
                
                rStart = lEnd + op.Length;
                rEnd = rStart;
                
                size_t j = i+1;
                while (j < nbOps && al.CigarData[j].Type != 'N') {
                    
                    CigarOp rOp = al.CigarData[j++];
                    if (BamUtils::opFollowsReference(rOp.Type)) {
                        rEnd += rOp.Length;
                    }
                }
                
                shared_ptr<Intron> location(new Intron(refId, lEnd, rStart, 
                        strandSpecific ? strandFromBool(al.IsReverseStrand()) : UNKNOWN));
                
                // We should now have the complete junction location information
                JunctionMapIterator it = distinctJunctions.find(*location);
                
                // If we couldn't find this location in the hashmap, add a new
                // location / junction pair.  If we've seen this location before
                // then add this alignment to the existing junction
                if (it == distinctJunctions.end()) {
                    JunctionPtr junction(new Junction(location, lStart, rEnd));
                    junction->addJunctionAlignment(al);
                    distinctJunctions[*location] = junction;
                    junctionList.push_back(junction);
                }
                else {
                    
                    JunctionPtr junction = it->second;
                    junction->addJunctionAlignment(al);
                    junction->extendFlanks(lStart, rEnd);
                }
                
                // Check if we have fully processed the cigar or not.  If not, then
                // that means that this cigar contains additional junctions, so 
                // process those using recursion
                if (j < nbOps) {                    
                    addJunctions(al, i+1, rStart, strandSpecific);
                    break;
                }                
            }
            else if (BamUtils::opFollowsReference(op.Type)) {
                lEnd += op.Length;                
            }
            else {
                
                // Insertions, should be ignored
                // if (op.Type == 'I')
                
                // Ignore any other op types not already covered
            }
        }
        
        // Record alignment name in map
        if (foundJunction) {            
            
            string name = BamUtils::deriveName(al);
            splicedAlignmentMap[name]++;
        }
        
        return foundJunction;        
    }
    
    /**
     * This will look for donor acceptor sites and find hamming distances around
     * the junctions.  In both cases we need to consult the genome, so both
     * parts of the junction analysis are handled in this function
     * @param genomeMapper
     * @param refs
     * @return 
     */
    void scanReference(GenomeMapper* genomeMapper, RefVector& refs) {
        
        auto_cpu_timer timer(1, " = Wall time taken: %ws\n\n");        
        
        cout << " - Acquiring junction sequence sites from genome ... ";
        cout.flush();
        
        uint64_t canonicalSites = 0;
        uint64_t semiCanonicalSites = 0;
        uint64_t nonCanonicalSites = 0;
        for(JunctionPtr j : junctionList) {
            
            CanonicalSS css = j->processJunctionWindow(genomeMapper, refs);
            
            switch(css) {
                case CANONICAL:
                    canonicalSites++;
                    break;
                case SEMI_CANONICAL:
                    semiCanonicalSites++;
                    break;
                case NO:
                    nonCanonicalSites++;
                    break;
            }
                     
        }
        
        cout << "done." << endl
             << " - Found " << canonicalSites << " canonical splice sites." << endl
             << " - Found " << semiCanonicalSites << " semi-canonical splice sites." << endl
             << " - Found " << nonCanonicalSites << " non-canonical splice sites." << endl;
    }
    
    void findFlankingAlignments(string alignmentsFile, bool strandSpecific) {
        
        auto_cpu_timer timer(1, " = Wall time taken: %ws\n\n");    
        
        cout << " - Using unspliced alignments file: " << alignmentsFile << endl
             << " - Acquiring all alignments in each junction's vicinity ... ";
        cout.flush();
                
        BamReader reader;
        
        if (!reader.Open(alignmentsFile)) {
            BOOST_THROW_EXCEPTION(JunctionException() << JunctionErrorInfo(string(
                    "Could not open bam reader for alignments file: ") + alignmentsFile));
        }
        // Sam header and refs info from the input bam
        SamHeader header = reader.GetHeader();
        RefVector refs = reader.GetReferenceData();

        // Opens the index for this BAM file
        string indexFile = alignmentsFile + ".bti";
        if ( !reader.OpenIndex(indexFile) ) {            
            if ( !reader.CreateIndex(BamIndex::BAMTOOLS) ) {
                BOOST_THROW_EXCEPTION(JunctionException() << JunctionErrorInfo(string(
                        "Error creating BAM index for alignments file: ") + indexFile));
            }            
        }
        
        // Read the alignments around every junction and set appropriate metrics
        size_t count = 0;
        //cout << endl;
        for(JunctionPtr j : junctionList) {            
            j->processJunctionVicinity(
                    reader, 
                    refs[j->getIntron()->refId].RefLength, 
                    meanQueryLength, 
                    maxQueryLength,
                    strandSpecific);
            
            //cout << count++ << endl;
        }
        
        // Reset the reader for future use.
        reader.Close();
        
        cout << "done." << endl;
    }
    
    void calcCoverage(string alignmentsFile, bool strandSpecific) {
        
        auto_cpu_timer timer(1, " = Wall time taken: %ws\n\n");    
        
        cout << " - Using unspliced alignments from: " << alignmentsFile << endl
             << " - Calculating per base depth and junction coverage ... ";
        cout.flush();
        
        DepthParser dp(alignmentsFile, strandSpecific);
        
        vector<uint32_t> batch;
        
        size_t i = 0;
        while(dp.loadNextBatch(batch)) {
            
            JunctionList subset;
            findJunctions(dp.getCurrentRefIndex(), subset);
            
            for(JunctionPtr j : subset) {
                j->calcCoverage(meanQueryLength, batch);
            }            
        }
        
        cout << "done." << endl;
    }
    
    void calcJunctionStats() {
        
        auto_cpu_timer timer(1, " = Wall time taken: %ws\n\n");    
        
        cout << " - Grouping junctions ... ";
        cout.flush();
        
        for(size_t i = 0; i < junctionList.size(); i++) {
        
            vector<JunctionPtr > junctionGroup;
            i = createJunctionGroup(i, junctionGroup);            
                        
            uint32_t maxReads = 0;
            size_t maxIndex = 0;
            bool uniqueJunction = junctionGroup.size() == 1;
            for(size_t j = 0; j < junctionGroup.size(); j++) {                    
                JunctionPtr junc = junctionGroup[j];
                if (maxReads < junc->getNbJunctionAlignments()) {
                    maxReads = junc->getNbJunctionAlignments();
                    maxIndex = j;
                }
                junc->setUniqueJunction(uniqueJunction);
            }
            junctionGroup[maxIndex]->setPrimaryJunction(true);
        }
        
        cout << "done." << endl;        
    }
    
    /**
     * Call this method to recalculate all junction metrics based on the current location
     * and alignment information present in this junction
     */
    void calcAllRemainingMetrics() {
       
        auto_cpu_timer timer(1, " = Wall time taken: %ws\n\n"); 
        
        cout << " - Calculating ... ";
        cout.flush();
        
        for(JunctionPtr j : junctionList) {
            j->calcAllRemainingMetrics(splicedAlignmentMap);
        }
        
        cout << "done." << endl;
    }
    
    void saveAll(string outputPrefix) {
        
        auto_cpu_timer timer(1, " = Wall time taken: %ws\n\n"); 
        
        string junctionReportPath = outputPrefix + ".junctions.txt";
        string junctionFilePath = outputPrefix + ".junctions.tab";
        string junctionGFFPath = outputPrefix + ".junctions.gff3";
        string junctionBEDAllPath = outputPrefix + ".junctions.all.bed";
        string junctionBEDCanonicalPath = outputPrefix + ".junctions.canonical.bed";
        string junctionBEDSemiCanonicalPath = outputPrefix + ".junctions.semicanonical.bed";
        string junctionBEDNovelPath = outputPrefix + ".junctions.novel.bed";
        
        cout << " - Saving junction report to: " << junctionReportPath << " ... ";
        cout.flush();
        
        // Print descriptive output to file
        ofstream junctionReportStream(junctionReportPath.c_str());
        outputDescription(junctionReportStream);
        junctionReportStream.close();

        cout << "done." << endl
             << " - Saving junction table to: " << junctionFilePath << " ... ";
        cout.flush();
        
        // Print junction stats to file
        ofstream junctionFileStream(junctionFilePath.c_str());
        junctionFileStream << (*this) << endl;
        junctionFileStream.close();
        
        cout << "done." << endl
             << " - Saving GFF file to: " << junctionGFFPath << " ... ";
        cout.flush();
        
        // Print junction stats to file
        ofstream junctionGFFStream(junctionGFFPath.c_str());
        outputGFF(junctionGFFStream);
        junctionGFFStream.close();
        
        // Output BED files
        
        cout << "done." << endl
             << " - Saving BED file with all junctions to: " << junctionBEDAllPath << " ... ";
        cout.flush();
        
        // Print junctions in BED format to file
        outputBED(junctionBEDAllPath, ALL);
        
        cout << "done." << endl
             << " - Saving BED file with canonical junctions to: " << junctionBEDCanonicalPath << " ... ";
        cout.flush();
        
        outputBED(junctionBEDCanonicalPath, CANONICAL);
        
        cout << "done." << endl
             << " - Saving BED file with semi canonical junctions to: " << junctionBEDSemiCanonicalPath << " ... ";
        cout.flush();
        
        outputBED(junctionBEDSemiCanonicalPath, SEMI_CANONICAL);
        
        cout << "done." << endl
             << " - Saving BED file with non-canonical (novel) junctions to: " << junctionBEDNovelPath << " ... ";
        cout.flush();
        
        outputBED(junctionBEDNovelPath, NO);
        
        cout << "done." << endl;
    }
    
    void outputDescription(std::ostream &strm) {
        
        uint64_t i = 0;
        for(JunctionPtr j : junctionList) {
            strm << "Junction " << i++ << ":" << endl;
            j->outputDescription(strm);
            strm << endl;
        }        
    }
    
    friend std::ostream& operator<<(std::ostream &strm, const JunctionSystem& js) {
        
        strm << "index\t" << Junction::junctionOutputHeader() << endl;
        
        uint64_t i = 0;
        for(JunctionPtr j : js.junctionList) {
            strm << i++ << "\t" << *j << endl;
        }
        
        return strm;
    }
    
    void outputGFF(std::ostream &strm) {
        
        uint64_t i = 0;
        for(JunctionPtr j : junctionList) {
            j->outputGFF(strm, i++, refs);
        }
    }
    
    void outputGTF(std::ostream &strm) {
        
    }
    
    void outputBED(string& path, CanonicalSS type) {
        
        ofstream junctionBEDStream(path.c_str());
        outputBED(junctionBEDStream, type);
        junctionBEDStream.close();
    }
    
    void outputBED(std::ostream &strm, CanonicalSS type) {
        strm << "track name=\"junctions\"" << endl;
        uint64_t i = 0;
        for(JunctionPtr j : junctionList) {
            if (type == ALL || j->getSpliceSiteType() == type) {
                j->outputBED(strm, i++, refs);
            }
        }
    }
    
    void load(string junctionTabFile) {
        
        ifstream ifs(junctionTabFile.c_str());
        
        string line;
        // Loop through until end of file or we move onto the next ref seq
        while ( std::getline(ifs, line) ) {
            if ( !line.empty() ) {
                JunctionPtr j = Junction::parse(line);
            }
        }
        
        ifs.close();
    }
    
};
}