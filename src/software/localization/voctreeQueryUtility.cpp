#include <openMVG/sfm/sfm_data_io.hpp>
#include <openMVG/sfm/pipelines/RegionsIO.hpp>
#include <openMVG/voctree/database.hpp>
#include <openMVG/voctree/databaseIO.hpp>
#include <openMVG/voctree/vocabulary_tree.hpp>
#include <openMVG/voctree/descriptor_loader.hpp>
#include <openMVG/matching/indMatch.hpp>
#include <openMVG/logger.hpp>
#include <openMVG/types.hpp>
#include <openMVG/voctree/databaseIO.hpp>
#include <openMVG/sfm/sfm_data.hpp>
#include <openMVG/sfm/sfm_data_io.hpp>
#include <openMVG/sfm/pipelines/RegionsIO.hpp>
#include <openMVG/sfm/pipelines/sfm_engine.hpp>
#include <openMVG/features/FeaturesPerView.hpp>
#include <openMVG/features/RegionsPerView.hpp>

#include <Eigen/Core>

#include <boost/program_options.hpp> 
#include <boost/accumulators/accumulators.hpp>
#include <boost/accumulators/statistics/tail.hpp>

#include <iostream>
#include <fstream>
#include <ostream>
#include <string>
#include <chrono>
#include <iomanip>


static const int DIMENSION = 128;

using namespace std;
using namespace boost::accumulators;
namespace bpo = boost::program_options;
namespace bfs = boost::filesystem;

typedef openMVG::features::Descriptor<float, DIMENSION> DescriptorFloat;
typedef openMVG::features::Descriptor<unsigned char, DIMENSION> DescriptorUChar;

std::ostream& operator<<(std::ostream& os, const openMVG::voctree::DocMatches &matches)
{
  os << "[ ";
  for(const auto &e : matches)
  {
    os << e.id << ", " << e.score << "; ";
  }
  os << "];\n";
  return os;
}

std::ostream& operator<<(std::ostream& os, const openMVG::voctree::Document &doc)
{
  os << "[ ";
  for(const openMVG::voctree::Word &w : doc)
  {
    os << w << ", ";
  }
  os << "];\n";
  return os;
}

std::string myToString(std::size_t i, std::size_t zeroPadding)
{
  stringstream ss;
  ss << std::setw(zeroPadding) << std::setfill('0') << i;
  return ss.str();
}

bool saveSparseHistogramPerImage(const std::string &filename, const openMVG::voctree::SparseHistogramPerImage &docs)
{
  std::ofstream fileout(filename);
  if(!fileout.is_open())
    return false;

  for(const auto& d: docs)
  {
    fileout << "d{" << d.first << "} = [";
    for(const auto& i: d.second)
      fileout << i.first << ", ";
    fileout << "]\n";
  }

  fileout.close();
  return true;
}

static const std::string programDescription =
        "This program is used to create a database with a provided dataset of image descriptors using a trained vocabulary tree.\n "
        "The database is then queried optionally with another set of images in order to retrieve for each image the set of most similar images in the dataset\n"
        "If another set of images is not provided, the program will perform a sanity check of the database by querying the database using the same images used to build it\n"
        "It takes as input either a list.txt file containing the a simple list of images (bundler format and older OpenMVG version format)\n"
        "or a sfm_data file (JSON) containing the list of images. In both cases it is assumed that the .desc to load are in the same directory as the input file\n"
        "For the vocabulary tree, it takes as input the input.tree (and the input.weight) file generated by createVoctree\n"
        "As a further output option (--outdir), it is possible to specify a directory in which it will create, for each query image (be it a query image of querylist or an image of keylist)\n"
        "it creates a directory with the same name of the image, inside which it creates a list of symbolic links to all the similar images found. The symbolic link naming convention\n"
        "is matchNumber.filename, where matchNumber is the relevant position of the image in the list of matches ([0-r]) and filename is its image file (eg image.jpg)\n";
/*
 * This program is used to create a database with a provided dataset of image descriptors using a trained vocabulary tree
 * The database is then queried with the same images in order to retrieve for each image the set of most similar images in the dataset
 */
int main(int argc, char** argv)
{
  /// verbosity level
  int verbosity = 1;
  /// the filename for the voctree weights
  std::string weightsName;
  /// flag for the optional weights file
  bool withWeights = false;
  /// the filename of the voctree
  std::string treeName;
  /// the file containing the list of features to use to build the database
  std::string keylist;
  /// the file containing the list of features to use as query
  std::string queryList = "";
  /// the file in which to save the results
  std::string outfile;
  /// the directory in which save the symlinks of the similar images
  std::string outDir;
  /// the file where to save the document map in matlab format
  std::string documentMapFile;
  std::string describerMethod = "SIFT";
  /// flag for the optional output file
  bool withOutput = false;
  /// flag for the optional output directory to save the symlink of the similar images
  bool withOutDir = false;
  /// it produces an output readable by matlab
  bool withQuery = false;
  /// it produces an output readable by matlab
  bool matlabOutput = false;
  /// the number of matches to retrieve for each image
  std::size_t numImageQuery = 10;
  std::string distance;
  int Nmax = 0;

  openMVG::sfm::SfM_Data sfmdata;
  openMVG::sfm::SfM_Data *sfmdataQuery;

  bpo::options_description desc(programDescription);
  desc.add_options()
          ("help,h", "Print this message")
          ("verbose,v", bpo::value<int>(&verbosity)->default_value(1), 
              "Verbosity level, 0 to mute")
          ("weights,w", bpo::value<std::string>(&weightsName), 
              "Input name for the weight file, if not provided the weights will be computed on the database built with the provided set")
          ("tree,t", bpo::value<std::string>(&treeName)->required(), 
              "Input name for the tree file")
          ("keylist,l", bpo::value<std::string>(&keylist)->required(), 
              "Path to the list file generated by OpenMVG containing the features to use for building the database")
          ("querylist,q", bpo::value<std::string>(&queryList),
              "Path to the list file to be used for querying the database")
          ("saveDocumentMap", bpo::value<std::string>(&documentMapFile),
              "A matlab file .m where to save the document map of the created database.")
          ("outdir", bpo::value<std::string>(&outDir),
              "Path to the directory in which save the symlinks of the similar images (it will be create if it does not exist)")
          ("describerMethod,m", bpo::value<std::string>(&describerMethod)->default_value(describerMethod),
              "method to use to describe an image")   
          ("results,r", bpo::value<std::size_t>(&numImageQuery)->default_value(numImageQuery),
              "The number of matches to retrieve for each image, 0 to retrieve all the images")
          ("matlab,", bpo::bool_switch(&matlabOutput)->default_value(matlabOutput),
              "It produces an output readable by matlab")
          ("outfile,o", bpo::value<std::string>(&outfile),
              "Name of the output file")
          ("Nmax,n", bpo::value<int>(&Nmax)->default_value(Nmax),
              "Number of features extracted from the .feat files")
          ("distance,d",bpo::value<std::string>(&distance)->default_value("strongCommonPoints"),
            "Distance used");


  bpo::variables_map vm;

  try
  {
    bpo::store(bpo::parse_command_line(argc, argv, desc), vm);

    if(vm.count("help") || (argc == 1))
    {
      std::cout << desc << std::endl;
      return EXIT_SUCCESS;
    }

    bpo::notify(vm);
  }
  catch(bpo::required_option& e)
  {
    std::cerr << "ERROR: " << e.what() << std::endl << std::endl;
    std::cout << "Usage:\n\n" << desc << std::endl;
    return EXIT_FAILURE;
  }
  catch(bpo::error& e)
  {
    std::cerr << "ERROR: " << e.what() << std::endl << std::endl;
    std::cout << "Usage:\n\n" << desc << std::endl;
    return EXIT_FAILURE;
  }

  if(vm.count("weights"))
  {
    withWeights = true;
  }
  if(vm.count("outfile"))
  {
    withOutput = true;
  }
  if(vm.count("querylist"))
  {
    withQuery = true;
  }
  if(vm.count("outdir"))
  {
    // check that both query list or klist are a json file
    withOutDir = bfs::path(keylist).extension().string() == ".json";
    if(withOutDir && withQuery)
    {
      withOutDir = withOutDir && ( bfs::path(queryList).extension().string() == ".json");
    }
  }


  //************************************************
  // Load vocabulary tree
  //************************************************

  OPENMVG_COUT("Loading vocabulary tree\n");
  openMVG::voctree::VocabularyTree<DescriptorFloat> tree(treeName);
  OPENMVG_COUT("tree loaded with\n\t" 
          << tree.levels() << " levels\n\t" 
          << tree.splits() << " branching factor");


  //************************************************
  // Create the database
  //************************************************

  OPENMVG_COUT("Creating the database...");
  // Add each object (document) to the database
  openMVG::voctree::Database db(tree.words());

  if(withWeights)
  {
    OPENMVG_COUT("Loading weights...");
    db.loadWeights(weightsName);
  }
  else
  {
    OPENMVG_COUT("No weights specified, skipping...");
  }


  //*********************************************************
  // Read the descriptors and populate the database
  //*********************************************************

  OPENMVG_COUT("Reading descriptors from " << keylist);
  auto detect_start = std::chrono::steady_clock::now();
  std::size_t numTotFeatures = openMVG::voctree::populateDatabase<DescriptorUChar>(keylist, tree, db, Nmax);
  auto detect_end = std::chrono::steady_clock::now();
  auto detect_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(detect_end - detect_start);

  if(numTotFeatures == 0)
  {
    OPENMVG_CERR("No descriptors loaded!!");
    return EXIT_FAILURE;
  }

  OPENMVG_COUT("Done! " << db.getSparseHistogramPerImage().size() << " sets of descriptors read for a total of " << numTotFeatures << " features");
  OPENMVG_COUT("Reading took " << detect_elapsed.count() << " sec");
  
  if(vm.count("saveDocumentMap"))
  {
    saveSparseHistogramPerImage(documentMapFile, db.getSparseHistogramPerImage());
  }

  if(!withWeights)
  {
    // If we don't have an input weight file, we compute weights based on the
    // current database.
    OPENMVG_COUT("Computing weights...");
    db.computeTfIdfWeights();
  }


  //************************************************
  // Query documents or sanity check
  //************************************************

  std::map<std::size_t, openMVG::voctree::DocMatches> allDocMatches;
  std::size_t wrong = 0;
  if(numImageQuery == 0)
  {
    // if 0 retrieve the score for all the documents of the database
    numImageQuery = db.size();
  }
  std::ofstream fileout;
  if(withOutput)
  {
    fileout.open(outfile, ofstream::out);
  }

  std::map<std::size_t, openMVG::voctree::SparseHistogram> histograms;

  // if the query list is not provided
  if(!withQuery)
  {
    // do a sanity check
    OPENMVG_COUT("Sanity check: querying the database with the same documents");
    db.sanityCheck(numImageQuery, allDocMatches);
  }
  else
  {
    // otherwise query the database with the provided query list
    OPENMVG_COUT("Querying the database with the documents in " << queryList);
    openMVG::voctree::queryDatabase<DescriptorUChar>(queryList, tree, db, numImageQuery, allDocMatches, histograms, distance, Nmax);
  }

  if(withOutDir)
  {
    // load the json for the dataset used to build the database
    if(openMVG::sfm::Load(sfmdata, keylist, openMVG::sfm::ESfM_Data::VIEWS))
    {
      OPENMVG_COUT("SfM data loaded from " << keylist << " containing: ");
      OPENMVG_COUT("\tnumber of views      : " << sfmdata.GetViews().size());
    }
    else
    {
      OPENMVG_CERR("Could not load the sfm_data file " << keylist << "!");
      return EXIT_FAILURE;
    }
    // load the json for the dataset used to query the database
    if(withQuery)
    {
      sfmdataQuery = new openMVG::sfm::SfM_Data();
      if(openMVG::sfm::Load(*sfmdataQuery, queryList, openMVG::sfm::ESfM_Data::VIEWS))
      {
        OPENMVG_COUT("SfM data loaded from " << queryList << " containing: ");
        OPENMVG_COUT("\tnumber of views      : " << sfmdataQuery->GetViews().size());
      }
      else
      {
        OPENMVG_CERR("Could not load the sfm_data file " << queryList << "!");
        return EXIT_FAILURE;
      }
    }
    else
    {
      // otherwise sfmdataQuery is just a link to the dataset sfmdata
      sfmdataQuery = &sfmdata;
    }

    // create recursively the provided out dir
    if(!bfs::exists(bfs::path(outDir)))
    {
//      OPENMVG_COUT("creating directory" << outDir);
      bfs::create_directories(bfs::path(outDir));
    }

  }

  openMVG::sfm::SfM_Data sfmData;
  if (!openMVG::sfm::Load(sfmData, queryList, openMVG::sfm::ESfM_Data(openMVG::sfm::VIEWS|openMVG::sfm::INTRINSICS))) {
    std::cerr << std::endl
      << "The input SfM_Data file \""<< queryList << "\" cannot be read." << std::endl;
    return EXIT_FAILURE;
  }

  using namespace openMVG::features;
  std::string matchDir = queryList.substr(0, queryList.find_last_of("/\\"));;
  const std::string sImage_describer = stlplus::create_filespec(matchDir, "image_describer", "json");
  std::unique_ptr<Regions> regions_type = Init_region_type_from_file(sImage_describer);
  if (!regions_type)
  {
    std::cerr << "Invalid: "
      << sImage_describer << " regions type file." << std::endl;
    return EXIT_FAILURE;
  }
  // Load the corresponding RegionsPerView
  // Get imageDescriberMethodType
  EImageDescriberType describerType = EImageDescriberType_stringToEnum(describerMethod);
  
  if((describerType != EImageDescriberType::SIFT) &&
      (describerType != EImageDescriberType::SIFT_FLOAT))
  {
    OPENMVG_CERR("Invalid describer method." << std::endl);
    return EXIT_FAILURE;
  }
  
  
  openMVG::features::RegionsPerView regionsPerView;
  if(!openMVG::sfm::loadRegionsPerView(regionsPerView, sfmData, matchDir, {describerType}))
  {
    OPENMVG_CERR("Invalid regions." << std::endl);
    return EXIT_FAILURE;
  }
  
  openMVG::matching::PairwiseSimpleMatches allMatches;

  for(auto docMatches: allDocMatches)
  {
    const openMVG::voctree::DocMatches& matches = docMatches.second;
    bfs::path dirname;
    OPENMVG_COUT("Camera: " << docMatches.first);
    OPENMVG_COUT("query document " << docMatches.first << " has " << matches.size() << " matches\tBest " << matches[0].id << " with score " << matches[0].score);
    if(withOutput)
    {
      if(!matlabOutput)
      {
        fileout << "Camera: " << docMatches.first << std::endl;
      }
      else
      {
        fileout << "m{" << docMatches.first + 1 << "}=";
        fileout << matches;
      }
    }
    if(withOutDir)
    {
      // create a new directory inside outDir with the same name as the query image
      // the query image can be either from the dataset or from the query list if provided

      // to put a symlink to the query image too
      bfs::path absoluteFilename; //< the abs path to the image
      bfs::path sylinkName; //< the name used for the symbolic link

      // get the dirname from the filename
      
      openMVG::sfm::Views::const_iterator it = sfmdataQuery->GetViews().find(docMatches.first);
      if(it == sfmdataQuery->GetViews().end())
      {
        // this is very wrong
        OPENMVG_CERR("Could not find the image file for the document " << docMatches.first << "!");
        return EXIT_FAILURE;
      }
      sylinkName = bfs::path(it->second->s_Img_path).filename();
      dirname = bfs::path(outDir) / sylinkName;
      absoluteFilename = bfs::path(sfmdataQuery->s_root_path) / sylinkName;
      bfs::create_directories(dirname);
      bfs::create_symlink(absoluteFilename, dirname / sylinkName);
      
      // Perform features matching
      const openMVG::voctree::SparseHistogram& currentHistogram = histograms.at(docMatches.first);
      
      for (const auto comparedPicture : matches)
      {
        openMVG::voctree::SparseHistogram comparedHistogram = histograms.at(comparedPicture.id);
        openMVG::Pair indexImagePair = openMVG::Pair(docMatches.first, comparedPicture.id);
        
        //Get the regions for the current view pair.
//        const openMVG::features::SIFT_Regions& lRegions = dynamic_cast<openMVG::features::SIFT_Regions>(regionsPerView->getRegions(indexImagePair.first);
//        const openMVG::features::SIFT_Regions& rRegions = dynamic_cast<openMVG::features::SIFT_Regions>(regionsPerView->getRegions(indexImagePair.second);
        
        //Distances Vector
        //const std::vector<float> distances;
        
        openMVG::matching::IndMatches featureMatches;

        for (const auto& currentLeaf: currentHistogram)
        {
          if ( (currentLeaf.second.size() == 1) )
          {
            auto leafRightIt = comparedHistogram.find(currentLeaf.first);
            if (leafRightIt == comparedHistogram.end())
              continue;
            if(leafRightIt->second.size() != 1)
              continue;

            const Regions& siftRegionsLeft = regionsPerView.getRegions(docMatches.first, describerType);
            const Regions& siftRegionsRight = regionsPerView.getRegions(comparedPicture.id, describerType);

            double dist = siftRegionsLeft.SquaredDescriptorDistance(currentLeaf.second[0], &siftRegionsRight, leafRightIt->second[0]);
            openMVG::matching::IndMatch currentMatch = openMVG::matching::IndMatch( currentLeaf.second[0], leafRightIt->second[0]
#ifdef OPENMVG_DEBUG_MATCHING
                    , dist
#endif
                    );
            featureMatches.push_back(currentMatch);

            // TODO: distance computation
          }
        }

        allMatches[indexImagePair] = featureMatches;

        // TODO: display + symlinks
      }
    }

    // now parse all the returned matches 
    for(std::size_t j = 0; j < matches.size(); ++j)
    {
      OPENMVG_COUT("\t match " << matches[j].id << " with score " << matches[j].score);
//      OPENMVG_CERR("" << i->first << " " << matches[j].id << " " << matches[j].score);
      if(withOutput && !matlabOutput) 
        fileout << docMatches.first << " " << matches[j].id << " " << matches[j].score << std::endl;

      if(withOutDir)
      {
        // create a new symbolic link inside the current directory pointing to
        // the relevant matching image
        bfs::path absoluteFilename; //< the abs path to the image
        bfs::path sylinkName; //< the name used for the symbolic link

        // get the dirname from the filename
        openMVG::sfm::Views::const_iterator it = sfmdata.GetViews().find(matches[j].id);
        if(it != sfmdata.GetViews().end())
        {
          bfs::path imgName(it->second->s_Img_path);
          sylinkName = bfs::path(myToString(j, 4) + "." + std::to_string(matches[j].score) + "." + imgName.filename().string());
          bfs::path imgPath(sfmdata.s_root_path);
          absoluteFilename = imgPath / imgName;
        }
        else
        {
          // this is very wrong
          OPENMVG_CERR("Could not find the image file for the document " << matches[j].id << "!");
          return EXIT_FAILURE;
        }
        bfs::create_symlink(absoluteFilename, dirname / sylinkName);
      }
    }

    if(!withQuery)
    {
      // only for the sanity check, check if the best matching image is the document itself
      if(docMatches.first != matches[0].id)
      {
        ++wrong;
        OPENMVG_COUT("##### wrong match for document " << docMatches.first);
      }
    }
  }

#ifdef OPENMVG_DEBUG_MATCHING
  std::cout << " ---------------------------- \n" << endl;
  std::cout << "Matching distances - Histogram: \n" << endl;
  std::map<int,int> stats;
  for( const auto& imgMatches: allMatches)
  {
    if(imgMatches.first.first == imgMatches.first.second)
      // Ignore auto-match
      continue;

    for( const openMVG::matching::IndMatch& featMatches: imgMatches.second)
    {
      int d = std::floor(featMatches._distance / 1000.0);
      if( stats.find(d) != stats.end() )
        stats[d] += 1;
      else
        stats[d] = 1;
    }
  }
  for(const auto& stat: stats)
  {
    std::cout << stat.first << "\t" << stat.second << std::endl;
  }
#endif

  if(!withQuery)
  {
    if(wrong)
      OPENMVG_COUT("there are " << wrong << " wrong matches");
    else
      OPENMVG_COUT("no wrong matches!");
  }

  if(withOutput)
  {
    fileout.close();
  }

  return EXIT_SUCCESS;
}
