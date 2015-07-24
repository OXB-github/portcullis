//  ********************************************************************
//  This file is part of Portcullis.
//
//  Portcullis is free software: you can redistribute it and/or modify
//  it under the terms of the GNU General Public License as published by
//  the Free Software Foundation, either version 3 of the License, or
//  (at your option) any later version.
//
//  Portcullis is distributed in the hope that it will be useful,
//  but WITHOUT ANY WARRANTY; without even the implied warranty of
//  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//  GNU General Public License for more details.
//
//  You should have received a copy of the GNU General Public License
//  along with Portcullis.  If not, see <http://www.gnu.org/licenses/>.
//  *******************************************************************

#include <fstream>
#include <iostream>
#include <memory>
#include <unordered_map>
#include <vector>
using std::cout;
using std::endl;
using std::ifstream;
using std::ofstream;
using std::shared_ptr;

#include <boost/algorithm/string/split.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/exception/all.hpp>
#include <boost/timer/timer.hpp>
using boost::split;
using boost::lexical_cast;
using boost::timer::auto_cpu_timer;

#include "bam/depth_parser.hpp"
using portcullis::bam::DepthParser;

#include "intron.hpp"
#include "junction.hpp"
#include "seq_utils.hpp"
using portcullis::Intron;
using portcullis::IntronHasher;
using portcullis::Junction;
using portcullis::JunctionPtr;
using portcullis::SeqUtils;

#include "junction_system.hpp"
    
size_t portcullis::JunctionSystem::createJunctionGroup(size_t index, vector<JunctionPtr>& group) {

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

void portcullis::JunctionSystem::findJunctions(const int32_t refId, JunctionList& subset) {

    subset.clear();
    for(JunctionPtr j : junctionList) {
        if (j->getIntron()->ref.index == refId) {
            subset.push_back(j);
        }    
    }        
}


portcullis::JunctionSystem::JunctionSystem() {
    minQueryLength = 0;
    meanQueryLength = 0.0;
    maxQueryLength = 0;        
}

portcullis::JunctionSystem::JunctionSystem(vector<RefSeq> refs) : JunctionSystem() {
    this->refs = refs;
}

portcullis::JunctionSystem::JunctionSystem(path junctionFile) : JunctionSystem() {
    load(junctionFile);
}

portcullis::JunctionSystem::~JunctionSystem() {
    distinctJunctions.clear();     
    junctionList.clear();
}

const JunctionList& portcullis::JunctionSystem::getJunctions() const {
    return junctionList;
}

size_t portcullis::JunctionSystem::size() {

    assert(distinctJunctions.size() == junctionList.size());

    return distinctJunctions.size();
}


void portcullis::JunctionSystem::setQueryLengthStats(int32_t min, double mean, int32_t max) {
    this->minQueryLength = min;
    this->meanQueryLength = mean;
    this->maxQueryLength = max;
}


bool portcullis::JunctionSystem::addJunction(JunctionPtr j) {

    j->clearAlignments();

    distinctJunctions[*(j->getIntron())] = j;
    junctionList.push_back(j);        
}

/**
 * Appends a new copy of all the junctions in the other junction system to this
 * junction system, without the Bam alignments associated with them.
 * @param other The other junctions system containing junctions to be added to this.
 */
void portcullis::JunctionSystem::append(JunctionSystem& other) {

    for(const auto& j : other.getJunctions()) {
        this->addJunction(j);
    }
}


bool portcullis::JunctionSystem::addJunctions(const BamAlignment& al, const size_t startOp, const int32_t offset, bool strandSpecific) {

    bool foundJunction = false;

    size_t nbOps = al.getNbCigarOps();

    int32_t refId = al.getReferenceId();
    string refName = refs[refId].name;
    int32_t refLength = refs[refId].length;
    int32_t lStart = offset;        
    int32_t lEnd = lStart;
    int32_t rStart = lStart;
    int32_t rEnd = lStart;

    for(size_t i = startOp; i < nbOps; i++) {

        CigarOp op = al.getCigarOpAt(i);
        
        // If first op is a soft clip, then move on
        if (i == startOp && op.type == BAM_CIGAR_SOFTCLIP_CHAR) {
            lStart = offset + op.length;        
            lEnd = lStart;
            rStart = lStart;
            rEnd = lStart;            
        }
        else if (op.type == BAM_CIGAR_REFSKIP_CHAR) {
            foundJunction = true;

            rStart = lEnd + op.length;
            rEnd = rStart;

            // Establish end position of right anchor in genomic coordinates
            size_t j = i+1;
            while (j < nbOps 
                    && rEnd < refLength 
                    && al.getCigarOpAt(j).type != BAM_CIGAR_REFSKIP_CHAR 
                    && al.getCigarOpAt(j).type != BAM_CIGAR_SOFTCLIP_CHAR) {

                CigarOp rOp = al.getCigarOpAt(j++);
                
                if (CigarOp::opConsumesReference(rOp.type)) {
                    rEnd += rOp.length;
                }
            }
            
            // The end of the flank will be one less than where we got to
            rEnd--;

            // Do some sanity checking... make sure there are not any strange N cigar ops that
            // drift over the edge of a reference sequence... seems like this can actually
            // happen in GSNAP!
            if (rStart - 1 >= refLength) {
                rStart = refLength;
            }            
            if (rEnd - 1 >= refLength) {
                rEnd = refLength - 1;
            }
            
            // Create the intron
            shared_ptr<Intron> location = make_shared<Intron>(RefSeq(refId, refName, refLength), lEnd, rStart - 1, 
                    strandSpecific ? strandFromBool(al.isReverseStrand()) : UNKNOWN);

            // We should now have the complete junction location information
            JunctionMapIterator it = distinctJunctions.find(*location);

            //cout << "Before junction add (inner): " << al.use_count() << endl;

            // If we couldn't find this location in the hashmap, add a new
            // location / junction pair.  If we've seen this location before
            // then add this alignment to the existing junction
            if (it == distinctJunctions.end()) {
                JunctionPtr junction = make_shared<Junction>(location, lStart, rEnd);
                junction->addJunctionAlignment(al);
                distinctJunctions[*location] = junction;
                junctionList.push_back(junction);                    
            }
            else {
                JunctionPtr junction = it->second;
                junction->addJunctionAlignment(al);                    
                junction->extendFlanks(lStart, rEnd);
            }

            // Don't try and go over the end of the reference
            // Might happen in some aligners like GSNAP or STAR
            if (rEnd >= refLength - 1) {
                break;
            }
            
            // Check if we have fully processed the cigar or not.  If not, then
            // that means that this cigar contains additional junctions, so 
            // process those using recursion
            if (j < nbOps) {                    
                addJunctions(al, i+1, rStart, strandSpecific);
                break;
            }
        }
        else if (CigarOp::opConsumesReference(op.type)) {
            lEnd += op.length;
        }
        
        // Ignore any other op types not already covered        
    }

    return foundJunction;        
}


void portcullis::JunctionSystem::findFlankingAlignments(const path& alignmentsFile, StrandSpecific strandSpecific, bool verbose) {

    auto_cpu_timer timer(1, " = Wall time taken: %ws\n\n");    

    cout << " - Using unspliced alignments file: " << alignmentsFile << endl
         << " - Acquiring all alignments in each junction's vicinity ... ";
    cout.flush();

    // Maybe try to multi-thread this part        
    BamReader reader(alignmentsFile, 1);

    // Open the file
    reader.open();

    // Read the alignments around every junction and set appropriate metrics
    size_t count = 0;
    //cout << endl;
    for(JunctionPtr j : junctionList) {            
        j->processJunctionVicinity(
                reader, 
                j->getIntron()->ref.length, 
                meanQueryLength, 
                maxQueryLength,
                strandSpecific);

        //cout << count++ << endl;
    }

    cout << "done." << endl;
}

void portcullis::JunctionSystem::calcCoverage(const path& alignmentsFile, StrandSpecific strandSpecific) {

    auto_cpu_timer timer(1, " = Wall time taken: %ws\n\n");    

    cout << " - Using unspliced alignments from: " << alignmentsFile << endl
         << " - Calculating per base depth and junction coverage ... ";
    cout.flush();

    DepthParser dp(alignmentsFile, static_cast<uint8_t>(strandSpecific), false);

    vector<uint32_t> batch;

    size_t i = 0;
    while(dp.loadNextBatch(batch)) {

        JunctionList subset;
        findJunctions(dp.getCurrentRefIndex(), subset);

        for(JunctionPtr j : subset) {
            j->calcCoverage(batch);
        }            
    }

    cout << "done." << endl;
}


void portcullis::JunctionSystem::calcMultipleMappingStats(SplicedAlignmentMap& map, bool verbose) {

    for(JunctionPtr j : junctionList) {
        j->calcMultipleMappingScore(map);
    }        
}


void portcullis::JunctionSystem::calcJunctionStats(bool verbose) {

    if (verbose) {
        auto_cpu_timer timer(1, " = Wall time taken: %ws\n\n");    

        cout << " - Grouping junctions ... ";
        cout.flush();
    }

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

    if(verbose) {
        cout << "done." << endl;        
    }
}


void portcullis::JunctionSystem::saveAll(const path& outputPrefix) {

    auto_cpu_timer timer(1, " = Wall time taken: %ws\n\n"); 

    string junctionReportPath = outputPrefix.string() + ".junctions.txt";
    string junctionFilePath = outputPrefix.string() + ".junctions.tab";
    string junctionGFFPath = outputPrefix.string() + ".junctions.gff3";
    string intronGFFPath = outputPrefix.string() + ".introns.gff3";
    string junctionBEDAllPath = outputPrefix.string() + ".junctions.bed";

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
         << " - Saving junction GFF file to: " << junctionGFFPath << " ... ";
    cout.flush();

    // Print junction stats to file
    ofstream junctionGFFStream(junctionGFFPath.c_str());
    outputJunctionGFF(junctionGFFStream);
    junctionGFFStream.close();

    cout << "done." << endl
         << " - Saving intron GFF file to: " << intronGFFPath << " ... ";
    cout.flush();

    // Print junction stats to file
    ofstream intronGFFStream(intronGFFPath.c_str());
    outputIntronGFF(intronGFFStream);
    intronGFFStream.close();

    // Output BED files

    cout << "done." << endl
         << " - Saving BED file with all junctions to: " << junctionBEDAllPath << " ... ";
    cout.flush();

    // Print junctions in BED format to file
    outputBED(junctionBEDAllPath, ALL);

    cout << "done." << endl;
}

void portcullis::JunctionSystem::outputDescription(std::ostream &strm) {

    uint64_t i = 0;
    for(JunctionPtr j : junctionList) {
        strm << "Junction " << i++ << ":" << endl;
        j->outputDescription(strm);
        strm << endl;
    }        
}

void portcullis::JunctionSystem::outputJunctionGFF(std::ostream &strm) {

    uint64_t i = 0;
    for(JunctionPtr j : junctionList) {
        j->outputJunctionGFF(strm, i++);
    }
}

void portcullis::JunctionSystem::outputIntronGFF(std::ostream &strm) {

    uint64_t i = 0;
    for(JunctionPtr j : junctionList) {
        j->outputIntronGFF(strm, i++);
    }
}


void portcullis::JunctionSystem::outputBED(string& path, CanonicalSS type) {

    ofstream junctionBEDStream(path.c_str());
    outputBED(junctionBEDStream, type);
    junctionBEDStream.close();
}

void portcullis::JunctionSystem::outputBED(std::ostream &strm, CanonicalSS type) {
    strm << "track name=\"junctions\"" << endl;
    uint64_t i = 0;
    for(JunctionPtr j : junctionList) {
        if (type == ALL || j->getSpliceSiteType() == type) {
            j->outputBED(strm, i++);
        }
    }
}

void portcullis::JunctionSystem::load(const path& junctionTabFile) {

    ifstream ifs(junctionTabFile.c_str());

    string line;
    // Loop through until end of file or we move onto the next ref seq
    while ( std::getline(ifs, line) ) {
        boost::trim(line);
        if ( !line.empty() && line.find("index") == std::string::npos ) {
            shared_ptr<Junction> j = Junction::parse(line);
            junctionList.push_back(j);
            distinctJunctions[*(j->getIntron())] = j;
        }
    }

    ifs.close();
}


JunctionPtr portcullis::JunctionSystem::getJunction(Intron& intron) const {
    try {
        return this->distinctJunctions.at(intron);
    }
    catch (std::out_of_range ex) {
        return nullptr;
    }
}

