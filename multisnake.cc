#include <fstream>
#include <iomanip>
#include "multisnake.h"
#include "itkImageFileReader.h"
#include "itkImageFileWriter.h"
#include "itkBSplineInterpolateImageFunction.h"
#include "itkResampleImageFilter.h"
#include "itkShiftScaleImageFilter.h"
#include "itkGradientImageFilter.h"
#include "itkVectorCastImageFilter.h"
#include "itkDiscreteGaussianImageFilter.h"
#include "itkCastImageFilter.h"
#include "itkGradientRecursiveGaussianImageFilter.h"
#include "itkImageRegionIteratorWithIndex.h"
#include "itkImageRegionIterator.h"
#include "itkOtsuThresholdImageFilter.h"
#include "itkOtsuMultipleThresholdsImageFilter.h"
#include "itkRescaleIntensityImageFilter.h"
#include "itkNormalVariateGenerator.h"
#include "itkMinimumMaximumImageCalculator.h"
#include "itkInvertIntensityImageFilter.h"
#include "itkRegionOfInterestImageFilter.h"
#include "itkPasteImageFilter.h"
#include "itkStatisticsImageFilter.h"
#include "itkImageDuplicator.h"
#include "itkAbsoluteValueDifferenceImageFilter.h"
#include "itkSquaredDifferenceImageFilter.h"
#include "itkSquareImageFilter.h"
#include "solver_bank.h"
#include "utility.h"

namespace soax {

Multisnake::Multisnake() : image_(NULL), external_force_(NULL),
                           intensity_scaling_(0.0), sigma_(1.0),
                           ridge_threshold_(0.01), foreground_(65535),
                           background_(0), initialize_z_(false),
                           is_2d_(false) {
  interpolator_ = InterpolatorType::New();
  vector_interpolator_ = VectorInterpolatorType::New();
  transform_ = TransformType::New();
  solver_bank_ = new SolverBank;
}

Multisnake::~Multisnake() {
  this->ClearSnakeContainer(initial_snakes_);
  // this->ClearSnakeContainer(converged_snakes_);
  converged_snakes_.clear();
  this->ClearSnakeContainer(comparing_snakes1_);
  this->ClearSnakeContainer(comparing_snakes2_);
  this->ClearSnakeContainerSequence(converged_snakes_sequence_);
  image_sequence_.clear();
  converged_snakes_sequence_.clear();
  junctions_sequence_.clear();
  delete solver_bank_;
}

void Multisnake::Reset() {
  this->ClearSnakeContainer(initial_snakes_);
  if (converged_snakes_sequence_.empty()) {
    this->ClearSnakeContainer(converged_snakes_);
  } else {
    converged_snakes_.clear();
  }
  this->ClearSnakeContainer(comparing_snakes1_);
  this->ClearSnakeContainer(comparing_snakes2_);
  this->ClearSnakeContainerSequence(converged_snakes_sequence_);
  image_sequence_.clear();
  converged_snakes_sequence_.clear();

  junctions_.Reset();
  image_filename_ = "";
  image_ = NULL;
  external_force_ = NULL;
  solver_bank_->Reset();
}

void Multisnake::ResetContainers() {
  this->ClearSnakeContainer(initial_snakes_);
  this->ClearSnakeContainer(converged_snakes_);
  junctions_.Reset();
  solver_bank_->Reset(false);
}

void Multisnake::LoadImage(const std::string &filename) {
  image_filename_ = filename;

  // // Find out the pixel type of the image in file
  // typedef itk::ImageIOBase::IOComponentType  ScalarPixelType;

  // itk::ImageIOBase::Pointer io = itk::ImageIOFactory::CreateImageIO(
  //     filename.c_str(), itk::ImageIOFactory::ReadMode);

  // if( !io ) {
  //   std::cerr << "NO IMAGEIO WAS FOUND" << std::endl;
  //   return;
  // }

  // // Now that we found the appropriate ImageIO class, ask it to
  // // read the meta data from the image file.
  // io->SetFileName(filename);
  // io->ReadImageInformation();
  // ScalarPixelType pixel_type = io->GetComponentType();
  // std::cout << itk::ImageIOBase::GetComponentTypeAsString(pixel_type) << std::endl;
  // unsigned num_dimensions = io->GetNumberOfDimensions();
  // std::cout << num_dimensions << std::endl;

  typedef itk::ImageFileReader<ImageType> ReaderType;
  ReaderType::Pointer reader = ReaderType::New();
  reader->SetFileName(filename);

  try {
    reader->Update();
  } catch(itk::ExceptionObject &e) {
    std::cerr << "Exception caught when reading an image!" << std::endl;
    std::cerr << e << std::endl;
  }

  image_ = reader->GetOutput();

  const ImageType::SizeType &size =
      image_->GetLargestPossibleRegion().GetSize();
  std::cout << "Image size: " << size << std::endl;
  if (size[2] < 2) {
    is_2d_ = true;
  }
  interpolator_->SetInputImage(image_);

  this->set_intensity_scaling(intensity_scaling_);
}

void Multisnake::LoadImageSequence(const std::string &filename, int nslices) {
  typedef itk::ImageFileReader<ImageType> ReaderType;
  typedef itk::RegionOfInterestImageFilter<ImageType, ImageType>
      ExtractorType;
  ReaderType::Pointer reader = ReaderType::New();

  reader->SetFileName(filename);
  reader->Update();
  ImageType::SizeType input_size =
      reader->GetOutput()->GetLargestPossibleRegion().GetSize();
  ImageType::SizeType frame_size;
  frame_size[0] = input_size[0];
  frame_size[1] = input_size[1];
  frame_size[2] = nslices;
  ImageType::IndexType frame_index =
      reader->GetOutput()->GetLargestPossibleRegion().GetIndex();

  ImageType::PointType origin;
  origin.Fill(0.0);

  for (int i = 0; i < input_size[2]; i += nslices) {
    frame_index[2] = i;
    ImageType::RegionType frame_region;
    frame_region.SetSize(frame_size);
    frame_region.SetIndex(frame_index);
    ExtractorType::Pointer extractor = ExtractorType::New();
    extractor->SetInput(reader->GetOutput());
    extractor->SetRegionOfInterest(frame_region);
    extractor->Update();
    extractor->GetOutput()->SetOrigin(origin);
    image_sequence_.push_back(extractor->GetOutput());
  }
  image_filename_ = filename;
  this->SetImage(0);
  interpolator_->SetInputImage(image_);
  this->set_intensity_scaling(intensity_scaling_);
}

void Multisnake::SetImage(int index) {
  if (image_sequence_.empty()) return;
  image_ = image_sequence_[index];
  this->set_intensity_scaling(0.0);
  interpolator_->SetInputImage(image_);
}

std::string Multisnake::GetImageName(bool suffix) const {
  unsigned last_slash_pos = image_filename_.find_last_of("/\\");
  if (!suffix) {
    unsigned last_dot_pos = image_filename_.find_last_of(".");
    return image_filename_.substr(last_slash_pos+1,
                                  last_dot_pos-last_slash_pos-1);
  } else {
    return image_filename_.substr(last_slash_pos+1);
  }
}

PointType Multisnake::GetImageCenter() const {
  PointType center;
  center.Fill(0.0);
  if (image_) {
    ImageType::SizeType size = image_->GetLargestPossibleRegion().GetSize();
    for (unsigned i = 0; i < kDimension; ++i)
      center[i] = size[i] / 2.0;
  }
  return center;
}

ImageType::Pointer Multisnake::InterpolateImage(ImageType::Pointer img,
                                                double z_spacing) {
  typedef itk::BSplineInterpolateImageFunction<ImageType, double, double>
      InterpolatorType;
  InterpolatorType::Pointer interp = InterpolatorType::New();
  interp->SetSplineOrder(3);
  typedef itk::ResampleImageFilter<ImageType, ImageType> ResamplerType;
  ResamplerType::Pointer resampler = ResamplerType::New();
  resampler->SetInterpolator(interp);
  resampler->SetOutputOrigin(img->GetOrigin());
  resampler->SetOutputDirection(img->GetDirection());

  // const ImageType::SpacingType &input_spacing = img->GetSpacing();
  ImageType::SpacingType input_spacing;
  input_spacing.Fill(1.0);
  input_spacing[2] = z_spacing;
  img->SetSpacing(input_spacing);

  const ImageType::SizeType &input_size =
      img->GetLargestPossibleRegion().GetSize();
  ImageType::SpacingType output_spacing;
  output_spacing.Fill(1.0);
  ImageType::SizeType output_size;
  for (unsigned i = 0; i < kDimension; ++i) {
    output_size[i] = static_cast<ImageType::SizeValueType>(
        input_size[i] * input_spacing[i]);
  }

  resampler->SetOutputSpacing(output_spacing);
  resampler->SetSize(output_size);
  resampler->SetDefaultPixelValue(0.0);
  resampler->SetInput(img);
  resampler->Update();
  return resampler->GetOutput();
}


void Multisnake::SaveAsIsotropicImage(const std::string &filename,
                                      double z_spacing) {
  typedef itk::BSplineInterpolateImageFunction<ImageType, double, double>
      InterpolatorType;
  InterpolatorType::Pointer interp = InterpolatorType::New();
  interp->SetSplineOrder(3);
  typedef itk::ResampleImageFilter<ImageType, ImageType> ResamplerType;
  ResamplerType::Pointer resampler = ResamplerType::New();
  resampler->SetInterpolator(interp);
  resampler->SetOutputOrigin(image_->GetOrigin());
  resampler->SetOutputDirection(image_->GetDirection());

  // const ImageType::SpacingType &input_spacing = image_->GetSpacing();
  ImageType::SpacingType input_spacing;
  input_spacing.Fill(1.0);
  input_spacing[2] = z_spacing;
  image_->SetSpacing(input_spacing);

  const ImageType::SizeType &input_size =
      image_->GetLargestPossibleRegion().GetSize();
  ImageType::SpacingType output_spacing;
  output_spacing.Fill(1.0);
  ImageType::SizeType output_size;
  for (unsigned i = 0; i < kDimension; ++i) {
    output_size[i] = static_cast<ImageType::SizeValueType>(
        input_size[i] * input_spacing[i]);
  }

  resampler->SetOutputSpacing(output_spacing);
  resampler->SetSize(output_size);
  resampler->SetDefaultPixelValue(0.0);
  resampler->SetInput(image_);

  typedef itk::ImageFileWriter<ImageType> WriterType;
  WriterType::Pointer writer = WriterType::New();
  writer->SetFileName(filename);
  writer->SetInput(resampler->GetOutput());

  try {
    writer->Update();
  } catch( itk::ExceptionObject & exp ) {
    std::cerr << "Exception caught when write an image!" << std::endl;
    std::cerr << exp << std::endl;
  }
}

void Multisnake::SaveAsIsotropicSequence(const std::string &filename, double z_spacing) {
  for (int i = 0; i < image_sequence_.size(); i++) {
    std::cout << "Interpolating frame #" << i+1 << " ..." << std::endl;
    image_sequence_[i] = this->InterpolateImage(image_sequence_[i], z_spacing);
  }
  ImageType::Pointer result = ImageType::New();
  ImageType::IndexType start;
  start.Fill(0);
  ImageType::SizeType size = image_sequence_.back()->GetLargestPossibleRegion().GetSize();
  size[2] *= image_sequence_.size();

  ImageType::RegionType region;
  region.SetSize(size);
  region.SetIndex(start);
  result->SetRegions(region);
  result->Allocate();

  typedef itk::PasteImageFilter<ImageType, ImageType> PasterType;
  PasterType::Pointer paster = PasterType::New();

  for (int i = 0; i < image_sequence_.size(); i++) {
    paster->SetDestinationImage(result);
    paster->SetSourceImage(image_sequence_[i]);
    paster->SetSourceRegion(image_sequence_[i]->GetLargestPossibleRegion());
    ImageType::IndexType dest_index = start;
    dest_index[2] = i * image_sequence_[i]->GetLargestPossibleRegion().GetSize()[2];
    paster->SetDestinationIndex(dest_index);
    paster->Update();
    result = paster->GetOutput();
  }
  typedef itk::ImageFileWriter<ImageType> WriterType;
  WriterType::Pointer writer = WriterType::New();
  writer->SetFileName(filename);
  result->SetRegions(region);
  writer->SetInput(result);

  try {
    writer->Update();
  } catch( itk::ExceptionObject & exp ) {
    std::cerr << "Exception caught when write an sequence!" << std::endl;
    std::cerr << exp << std::endl;
  }
}

void Multisnake::LoadParameters(const std::string &filename) {
  std::ifstream infile(filename.c_str());
  if (!infile.is_open()) {
    std::cerr << "LoadParameters: couldn't open file: "
              << infile << std::endl;
    return;
  }

  std::string line;
  while (std::getline(infile, line)) {
    std::stringstream converter;
    converter << line;
    std::string name, value;
    converter >> name >> value;
    this->AssignParameters(name, value);
  }
}

void Multisnake::AssignParameters(const std::string &name,
                                  const std::string &value) {
  if (name == "intensity-scaling") {
    this->set_intensity_scaling(String2Double(value));
    Snake::set_intensity_scaling(intensity_scaling_);
  } else if (name == "smoothing") {
    sigma_ = String2Double(value);
  } else if (name == "grad-diff") {
    ridge_threshold_ = String2Double(value);
  } else if (name == "foreground") {
    foreground_ = String2Unsigned(value);
    Snake::set_foreground(foreground_);
  } else if (name == "background") {
    background_ = String2Unsigned(value);
    Snake::set_background(background_);
  } else if (name == "spacing") {
    Snake::set_desired_spacing(String2Double(value));
  } else if (name == "init-z") {
    initialize_z_ = value == "true";
  } else if (name == "minimum-size") {
    Snake::set_minimum_length(String2Double(value));
  } else if (name == "max-iterations") {
    Snake::set_max_iterations(String2Unsigned(value));
  } else if (name == "change-threshold") {
    Snake::set_change_threshold(String2Double(value));
  } else if (name == "check-period") {
    Snake::set_check_period(String2Unsigned(value));
  } else if (name == "alpha") {
    solver_bank_->set_alpha(String2Double(value));
  } else if (name == "beta") {
    solver_bank_->set_beta(String2Double(value));
  } else if (name == "gamma") {
    solver_bank_->set_gamma(String2Double(value));
    // Snake::set_gamma(solver_bank_->gamma());
  } else if (name == "weight") {
    Snake::set_external_factor(String2Double(value));
  } else if (name == "stretch") {
    Snake::set_stretch_factor(String2Double(value));
  } else if (name == "nsector") {
    Snake::set_number_of_sectors(String2Unsigned(value));
  } else if (name == "radial-near") {
    Snake::set_radial_near(String2Unsigned(value));
  } else if (name == "radial-far") {
    Snake::set_radial_far(String2Unsigned(value));
  } else if (name == "delta") {
    Snake::set_delta(String2Unsigned(value));
  } else if (name == "overlap-threshold") {
    Snake::set_overlap_threshold(String2Double(value));
  } else if (name == "grouping-distance-threshold") {
    Snake::set_grouping_distance_threshold(String2Double(value));
  } else if (name == "grouping-delta") {
    Snake::set_grouping_delta(String2Unsigned(value));
  } else if (name == "direction-threshold") {
    Snake::set_direction_threshold(String2Double(value));
  } else if (name == "damp-z") {
    Snake::set_damp_z(value == "true");
  }
}


void Multisnake::SaveParameters(const std::string &filename) const {
  std::ofstream outfile;
  outfile.open(filename.c_str());
  if (!outfile.is_open()) {
    std::cerr << "Couldn't open file: " << outfile << std::endl;
    return;
  }
  this->WriteParameters(outfile);
  outfile.close();
}

void Multisnake::WriteParameters(std::ostream &os) const {
  os << std::boolalpha;
  os << "intensity-scaling\t" << intensity_scaling_ << std::endl;
  os << "smoothing\t" << sigma_ << std::endl;
  os << "grad-diff\t" << ridge_threshold_ << std::endl;
  os << "foreground\t" << foreground_ << std::endl;
  os << "background\t" << background_ << std::endl;
  os << "init-z\t" << initialize_z_ << std::endl;
  os << "spacing \t" << Snake::desired_spacing() << std::endl;
  os << "minimum-size \t" << Snake::minimum_length() << std::endl;
  os << "max-iterations \t" << Snake::max_iterations() << std::endl;
  os << "change-threshold \t" << Snake::change_threshold() << std::endl;
  os << "check-period \t" << Snake::check_period() << std::endl;
  os << "alpha \t" << solver_bank_->alpha() << std::endl;
  os << "beta \t" << solver_bank_->beta() << std::endl;
  os << "gamma \t" << solver_bank_->gamma() << std::endl;
  os << "weight \t" << Snake::external_factor() << std::endl;
  os << "stretch \t" << Snake::stretch_factor() << std::endl;
  os << "nsector \t" << Snake::number_of_sectors() << std::endl;
  os << "radial-near \t" << Snake::radial_near() << std::endl;
  os << "radial-far \t" << Snake::radial_far() << std::endl;
  os << "delta \t" << Snake::delta() << std::endl;
  os << "overlap-threshold \t" << Snake::overlap_threshold() << std::endl;
  os << "grouping-distance-threshold \t"
     << Snake::grouping_distance_threshold() << std::endl;
  os << "grouping-delta \t" << Snake::grouping_delta() << std::endl;
  os << "direction-threshold \t" << Snake::direction_threshold() << std::endl;
  os << "damp-z \t" << Snake::damp_z() << std::endl;
  os << std::noboolalpha;
}

void Multisnake::InvertImageIntensity() {
  ImageType::PixelType maximum = this->GetMaxImageIntensity();
  std::cout << "Previous Maximum intensity: " << maximum << std::endl;

  typedef itk::InvertIntensityImageFilter<ImageType> FilterType;
  FilterType::Pointer filter = FilterType::New();
  filter->SetInput(image_);
  filter->SetMaximum(maximum);

  // typedef itk::RescaleIntensityImageFilter<ImageType, ImageType>
  //     RescalerType;
  // RescalerType::Pointer rescaler = RescalerType::New();
  // rescaler->SetInput(filter->GetOutput());
  // rescaler->SetOutputMinimum(0);
  // rescaler->SetOutputMaximum(255);
  // rescaler->Update();
  // image_ = rescaler->GetOutput();
  // std::cout << "maximum: " << filter->GetMaximum() << std::endl;
  filter->Update();
  image_ = filter->GetOutput();
  interpolator_->SetInputImage(image_);
  maximum = this->GetMaxImageIntensity();
  std::cout << "Current Maximum intensity: " << maximum << std::endl;
}

void Multisnake::ComputeImageGradient(bool reset) {
  if (!reset && external_force_) return;
  external_force_ = NULL;

  typedef itk::Image<double, kDimension> InternalImageType;
  typedef itk::ShiftScaleImageFilter<ImageType, InternalImageType> ScalerType;
  ScalerType::Pointer scaler = ScalerType::New();
  scaler->SetInput(image_);
  scaler->SetScale(intensity_scaling_);
  scaler->SetShift(0.0);
  scaler->Update();

  if (is_2d_ || sigma_ < 0.01) { // no smoothing, needs to be fixed
                                 // for 2d image
    typedef itk::GradientImageFilter<InternalImageType> FilterType;
    FilterType::Pointer filter = FilterType::New();
    // filter->SetInput(image_);
    filter->SetInput(scaler->GetOutput());
    typedef itk::VectorCastImageFilter<FilterType::OutputImageType,
                                       VectorImageType> CasterType;
    CasterType::Pointer caster = CasterType::New();
    caster->SetInput(filter->GetOutput());
    try {
      caster->Update();
    } catch(itk::ExceptionObject & e) {
      std::cerr << "Exception caught when computing image gradient!\n"
                << e << std::endl;
    }
    external_force_ = caster->GetOutput();
    external_force_->DisconnectPipeline();
  } else {
    typedef itk::GradientRecursiveGaussianImageFilter<InternalImageType,
                                                      VectorImageType> FilterType;
    FilterType::Pointer filter = FilterType::New();
    filter->SetSigma(sigma_);
    // filter->SetInput(image_);
    filter->SetInput(scaler->GetOutput());
    try {
      filter->Update();
    } catch(itk::ExceptionObject & e) {
      std::cerr << "Exception caught when computing image gradient!\n"
                << e << std::endl;
    }
    external_force_ = filter->GetOutput();
    external_force_->DisconnectPipeline();
  }
  vector_interpolator_->SetInputImage(external_force_);
}

void Multisnake::ComputeImageGradientForSequence(int index) {
  this->SetImage(index);
  this->ComputeImageGradient();
}

void Multisnake::UpdateExternalForce() {
  typedef itk::Image<double, kDimension> InternalImageType;
  typedef itk::ShiftScaleImageFilter<ImageType, InternalImageType> ScalerType;
  ScalerType::Pointer scaler = ScalerType::New();
  scaler->SetInput(image_);
  scaler->SetScale(intensity_scaling_);
  scaler->SetShift(0.0);
  scaler->Update();

  typedef itk::GradientImageFilter<InternalImageType> FilterType;
  FilterType::Pointer filter = FilterType::New();
  filter->SetInput(scaler->GetOutput());
  typedef itk::VectorCastImageFilter<FilterType::OutputImageType,
                                     VectorImageType> CasterType;
  CasterType::Pointer caster = CasterType::New();
  caster->SetInput(filter->GetOutput());
  try {
    caster->Update();
  } catch(itk::ExceptionObject & e) {
    std::cerr << "Exception caught when updating external force!\n"
              << e << std::endl;
  }
  external_force_ = caster->GetOutput();
  external_force_->DisconnectPipeline();
  vector_interpolator_->SetInputImage(external_force_);
}

void Multisnake::InitializeSnakes() {
  this->ClearSnakeContainer(initial_snakes_);
  BoolVectorImageType::Pointer ridge_image =
      InitializeBoolVectorImage();
  this->ScanGradient(ridge_image);
  // this->UpdateExternalForce();
  unsigned num_directions = 2;
  if (!is_2d_ && initialize_z_) num_directions = 3;
  // unsigned num_directions = 1; // for blood vessels
  BoolVectorImageType::Pointer candidate_image =
      InitializeBoolVectorImage();

  for (unsigned d = 0; d < num_directions; ++d) {
    this->GenerateCandidates(ridge_image, candidate_image, d);
  }

  // std::ofstream ofs("x-ridges.txt");
  // this->PrintCandidatePoints(ridge_image, ofs, 0);

  for (unsigned d = 0; d < num_directions; ++d) {
    this->LinkCandidates(candidate_image, d);
  }
  std::sort(initial_snakes_.begin(), initial_snakes_.end(), IsShorter);
  // std::sort(initial_snakes_.begin(), initial_snakes_.end(), IsDarker);
  // std::sort(initial_snakes_.begin(), initial_snakes_.end(), IsBrighter);
  // this->PrintSnakes(initial_snakes_);
}

Multisnake::BoolVectorImageType::Pointer
Multisnake::InitializeBoolVectorImage() {
  BoolVectorImageType::Pointer image = BoolVectorImageType::New();
  image->SetRegions(image_->GetLargestPossibleRegion());
  image->Allocate();

  BoolVectorImageType::PixelType initial_flag;
  initial_flag.Fill(false);
  image->FillBuffer(initial_flag);

  return image;
}

void Multisnake::ScanGradient(BoolVectorImageType::Pointer ridge_image) {
  typedef itk::ImageRegionIteratorWithIndex<BoolVectorImageType>
      OutputIteratorType;

  OutputIteratorType iter(ridge_image,
                          ridge_image->GetLargestPossibleRegion());

  const unsigned d = is_2d_? 2 : 3;
  for (unsigned i = 0; i < d; ++i) {
    for (iter.GoToBegin(); !iter.IsAtEnd(); ++iter) {
      VectorImageType::IndexType index = iter.GetIndex();
      VectorImageType::IndexType current_index = index;

      unsigned cnt = 0;
      double grad_comp = external_force_->GetPixel(index)[i];
      // if (invert_intensity_) grad_comp = -grad_comp;

      if (grad_comp < ridge_threshold_) {
        continue;
      } else {
        while (true) {
          current_index[i]++;
          cnt++;

          if (!image_->GetLargestPossibleRegion().IsInside(current_index))
            break;

          double curr_grad_comp = external_force_->GetPixel(current_index)[i];
          // if (invert_intensity_) curr_grad_comp = - curr_grad_comp;

          if (curr_grad_comp > ridge_threshold_) {
            break;
          } else if (curr_grad_comp < -ridge_threshold_) {
            current_index[i] -= cnt/2;
            ridge_image->GetPixel(current_index)[i] = true;
            break;
          }
        }
      }
    }
  }
}

void Multisnake::GenerateCandidates(
    BoolVectorImageType::Pointer ridge_image,
    BoolVectorImageType::Pointer candidate_image, unsigned direction) {
  typedef itk::ImageRegionIteratorWithIndex<BoolVectorImageType>
      OutputIteratorType;
  typedef itk::ImageRegionConstIteratorWithIndex<ImageType>
      ConstIteratorType;

  OutputIteratorType iter(candidate_image,
                          candidate_image->GetLargestPossibleRegion());
  ConstIteratorType iter2(image_, image_->GetLargestPossibleRegion());

  for (iter.GoToBegin(), iter2.GoToBegin(); !iter.IsAtEnd();
       ++iter, ++iter2) {
    if (iter2.Value() > foreground_ || iter2.Value() < background_)
      continue;
    BoolVectorImageType::IndexType index = iter.GetIndex();
    BoolVectorImageType::PixelType ridge_value = ridge_image->GetPixel(index);

    unsigned d = is_2d_ ? 2 : 3;
    if (is_2d_) {
      iter.Value()[direction] = ridge_value[(direction+1) % d];
    } else {
      iter.Value()[direction] = ridge_value[(direction+1) % d] &&
          ridge_value[(direction+2) % d];
    }
  }
}

void Multisnake::PrintCandidatePoints(
    BoolVectorImageType::Pointer image, std::ostream &os,
    unsigned direction) const {
  typedef itk::ImageRegionIteratorWithIndex<BoolVectorImageType> IteratorType;
  IteratorType it(image, image->GetLargestPossibleRegion());
  for (it.GoToBegin(); !it.IsAtEnd(); ++it) {
    if (it.Value()[direction])
      os << it.GetIndex() << std::endl;
  }
}

void Multisnake::LinkCandidates(
    BoolVectorImageType::Pointer candidate_image, unsigned direction) {
  ImageType::SizeType size = image_->GetLargestPossibleRegion().GetSize();

  if (is_2d_) {
    for (unsigned c0 = 0; c0 < size[direction]; ++c0) {
      for (unsigned c1 = 0; c1 < size[(direction+1)%2]; ++c1) {
        BoolVectorImageType::IndexType current_index;
        current_index[direction] = c0;
        current_index[(direction+1)%2] = c1;
        current_index[2] = 0;

        if (candidate_image->GetPixel(current_index)[direction])
          LinkFromIndex(candidate_image, current_index, direction);
      }
    }
  } else {
    for (unsigned c0 = 0; c0 < size[direction]; ++c0) {
      for (unsigned c1 = 0; c1 < size[(direction+1)%3]; ++c1) {
        for (unsigned c2 = 0; c2 < size[(direction+2)%3]; ++c2) {
          BoolVectorImageType::IndexType current_index;
          current_index[direction] = c0;
          current_index[(direction+1)%3] = c1;
          current_index[(direction+2)%3] = c2;

          if (candidate_image->GetPixel(current_index)[direction])
            LinkFromIndex(candidate_image, current_index, direction);
        }
      }
    }
  }
}

void Multisnake::LinkFromIndex(
    BoolVectorImageType::Pointer candidate_image,
    BoolVectorImageType::IndexType& index, unsigned direction) {
  PointContainer candidates;

  while (true) {
    // if (!IsInsideImage(point)) break;
    if (!candidate_image->GetLargestPossibleRegion().IsInside(index))
      break;
    PointType point;
    point[0] = index[0];
    point[1] = index[1];
    point[2] = index[2];
    candidates.push_back(point);

    BoolVectorImageType::PixelType pixel_value;
    pixel_value[0] = pixel_value[1] = pixel_value[2] = false;
    candidate_image->SetPixel(index, pixel_value);
    //candidate_image->GetPixel(index)[direction] = false;

    bool next_found = FindNextCandidate(candidate_image, index, direction);
    if (!next_found) break;
  }

  if (candidates.size() > 1) {
    Snake *snake = new Snake(candidates, true, false, image_,
                             external_force_, interpolator_,
                             vector_interpolator_, transform_);
    snake->set_initial_state(true);
    snake->Resample();

    if (snake->viable())
      initial_snakes_.push_back(snake);
    else
      delete snake;
  }
}


bool Multisnake::FindNextCandidate(
    BoolVectorImageType::Pointer candidate_image,
    BoolVectorImageType::IndexType &index, unsigned direction) {
  BoolVectorImageType::IndexType current_index = index;
  index[direction]++;

  if (!candidate_image->GetLargestPossibleRegion().IsInside(index))
    return false;

  if (candidate_image->GetPixel(index)[direction])
    return true;

  if (is_2d_) {
    int d1 = (direction + 1) % 2;
    for (int c1 = current_index[d1] - 1;
         c1 <= current_index[d1] + 1; ++c1) {
      index[d1] = c1;
      index[2] = 0;
      if (candidate_image->GetLargestPossibleRegion().IsInside(index) &&
          candidate_image->GetPixel(index)[direction])
        return true;
    }
  } else {
    int d1 = (direction + 1) % 3;
    int d2 = (direction + 2) % 3;

    for (int c1 = current_index[d1] - 1;
         c1 <= current_index[d1] + 1; ++c1) {
      for (int c2 = current_index[d2] - 1;
           c2 <= current_index[d2] + 1; ++c2) {
        index[d1] = c1;
        index[d2] = c2;
        if (candidate_image->GetLargestPossibleRegion().IsInside(index) &&
            candidate_image->GetPixel(index)[direction])
          return true;
      }
    }
  }
  return false;
}

// void Multisnake::DeformSnakes(QProgressBar * progress_bar) {
void Multisnake::DeformSnakes() {
  unsigned ncompleted = 0;
  std::cout << "# initial snakes: " << initial_snakes_.size() << std::endl;
  // this->ClearSnakeContainer(converged_snakes_);
  converged_snakes_.clear();
  assert(converged_snakes_.empty());
  // std::ofstream outfile("initial-intensities.txt");
  while (!initial_snakes_.empty()) {
    Snake *snake = initial_snakes_.back();
    initial_snakes_.pop_back();
    // outfile << snake->ComputeIntensity() << std::endl;
    solver_bank_->Reset(false);
    snake->Evolve(solver_bank_, converged_snakes_, kBigNumber, is_2d_);

    if (snake->viable()) {
      converged_snakes_.push_back(snake);
    } else {
      initial_snakes_.insert(initial_snakes_.end(),
                             snake->subsnakes().begin(),
                             snake->subsnakes().end());
      delete snake;
    }

    ncompleted++;
    emit ExtractionProgressed(ncompleted);
    std::cout << "\rRemaining: " << std::setw(6)
              << initial_snakes_.size() << std::flush;
  }
  std::cout << "\n# Converged snakes: " << converged_snakes_.size() << std::endl;
}

void Multisnake::DeformSnakesForSequence() {
  std::cout << "============ Parameters for Sequence ============" << std::endl;
  this->WriteParameters(std::cout);
  std::cout << "=================================================" << std::endl;

  converged_snakes_sequence_.reserve(image_sequence_.size());

  for (int i = 0; i < image_sequence_.size(); i++) {
    this->SetImage(i);
    std::cout << "Frame #" << i << "\tintensity-scaling: " << intensity_scaling_
              << std::endl;

    if (i) {
      this->ComputeImageGradientForSequence(i);
      this->InitializeSnakes();
    }
    this->DeformSnakes();
    this->CutSnakesAtTJunctions();
    this->GroupSnakes();

    converged_snakes_sequence_.push_back(converged_snakes_);
    junctions_sequence_.push_back(junctions_.junction_points());
    junctions_.Reset();
    emit ExtractionCompleteForFrame(i+1);
  }
}

void Multisnake::CutSnakesAtTJunctions() {
  SnakeContainer segments;
  this->CutSnakes(segments);
  this->ClearSnakeContainer(converged_snakes_);
  converged_snakes_ = segments;
}

void Multisnake::CutSnakes(SnakeContainer &seg) {
  if (converged_snakes_.empty()) return;
  for (SnakeIterator it = converged_snakes_.begin();
       it != converged_snakes_.end(); ++it) {
    (*it)->UpdateHookedIndices();
  }

  for (SnakeIterator it = converged_snakes_.begin();
       it != converged_snakes_.end(); ++it) {
    (*it)->CopySubSnakes(seg);
  }
}

void Multisnake::ClearSnakeContainer(SnakeContainer &snakes) {
  if (snakes.empty()) return;
  for (SnakeContainer::iterator it = snakes.begin();
       it != snakes.end(); ++it) {
    // std::cout << "Deleting snakes ..." << std::endl;
    delete *it;
  }
  snakes.clear();
}

void Multisnake::ClearSnakeContainerSequence(std::vector<SnakeContainer>
                                             &snakes_sequence) {
  for (int i = 0; i < snakes_sequence.size(); i++) {
    this->ClearSnakeContainer(snakes_sequence[i]);
  }
}

void Multisnake::GroupSnakes() {
  junctions_.Initialize(converged_snakes_);
  junctions_.Union();
  junctions_.Configure();
  //  junctions_.PrintTipSets();
  //  junctions_.PrintTips();
  this->LinkSegments(converged_snakes_);
  SnakeIterator it = converged_snakes_.begin();
  while (it != converged_snakes_.end()) {
    solver_bank_->Reset(false);
    (*it)->EvolveWithTipFixed(solver_bank_, 100, is_2d_);
    if ((*it)->viable()) {
      it++;
    } else {
      delete *it;
      // std::cout << "snake  erased!" << std::endl;
      it = converged_snakes_.erase(it);
    }
  }
  this->UpdateJunctions();
}

void Multisnake::LinkSegments(SnakeContainer &seg) {
  SnakeContainer c;
  std::reverse(seg.begin(), seg.end());
  while (!seg.empty()) {
    Snake *segment = seg.back();
    seg.pop_back();
    // log is for detecting loop in linking snake segments
    SnakeSet log;
    PointContainer points;
    bool is_open = true;
    this->LinkFromSegment(segment, seg, log, points, is_open);
    delete segment;
    Snake *s = new Snake(points, is_open, false, image_,
                         external_force_, interpolator_,
                         vector_interpolator_, transform_);
    s->Resample();
    c.push_back(s);
  }
  converged_snakes_ = c;
}

void Multisnake::LinkFromSegment(Snake *s, SnakeContainer &seg,
                                 SnakeSet &log, PointContainer &pc,
                                 bool &is_open) {
  log.insert(s);
  pc = s->vertices();
  SnakeTip * t = junctions_.FindSnakeTip(s, true);
  this->LinkFromSegmentTip(t->neighbor(), pc, is_open, seg, log, true);
  t = junctions_.FindSnakeTip(s, false);
  this->LinkFromSegmentTip(t->neighbor(), pc, is_open, seg, log, false);
}

void Multisnake::LinkFromSegmentTip(SnakeTip *neighbor, PointContainer &pc,
                                    bool &is_open, SnakeContainer &seg,
                                    SnakeSet &log, bool from_head) {
  if (!neighbor) {
    return;
  } else if (log.find(neighbor->snake()) != log.end()) {
    is_open = false;
    return;
  } else {
    this->AddToPointContainer(pc, neighbor->snake(), neighbor->is_head(),
                              from_head);
    SnakeContainer::iterator it = std::find(seg.begin(), seg.end(),
                                            neighbor->snake());
    if (it != seg.end())
      seg.erase(it);
    //seg.remove(neighbor->snake());
    log.insert(neighbor->snake());
    delete neighbor->snake();
    SnakeTip * t = junctions_.FindSnakeTip(neighbor->snake(),
                                           !neighbor->is_head());
    this->LinkFromSegmentTip(t->neighbor(), pc, is_open, seg, log,
                             from_head);
  }
}

void Multisnake::AddToPointContainer(PointContainer &pc, Snake *s,
                                     bool is_head, bool from_head) {
  PointContainer p = s->vertices();

  if (from_head) {
    if (is_head)
      std::reverse(p.begin(), p.end());
    pc.insert(pc.begin(), p.begin(), p.end());
  } else {
    if (!is_head)
      std::reverse(p.begin(), p.end());
    pc.insert(pc.end(), p.begin(), p.end());
  }
}

void Multisnake::UpdateJunctions() {
  if (converged_snakes_.empty())
    junctions_.ClearJunctionPoints();

  const PointContainer &junction_points = junctions_.junction_points();
  PointContainer new_junction_points;
  for (PointConstIterator it = junction_points.begin();
       it != junction_points.end(); ++it) {
    unsigned num_of_close_snake = GetNumberOfSnakesCloseToPoint(*it);
    if (num_of_close_snake > 1)
      new_junction_points.push_back(*it);
  }
  // int size_diff = junction_points.size() - new_junction_points.size();
  // std::cout << "size diff: " << size_diff << std::endl;
  junctions_.set_junction_points(new_junction_points);
}

unsigned Multisnake::GetNumberOfSnakesCloseToPoint(const PointType &p) {
  unsigned num = 0;
  // TODO: improve this dist_threshold
  // const double dist_threshold = grouping_distance_threshold_/2;
  const double dist_threshold = Snake::grouping_distance_threshold();
  for (SnakeContainer::const_iterator it = converged_snakes_.begin();
       it != converged_snakes_.end(); ++it) {
    if ((*it)->PassThrough(p, dist_threshold))
      num++;
  }
  return num;
}

void Multisnake::LoadCurves(const char *filename, double *offset) {
  this->ClearSnakeContainer(comparing_snakes1_);
  std::ifstream infile(filename);
  if (!infile.is_open()) {
    std::cerr << "LoadCurves: couldn't open file: " << filename << std::endl;
    return;
  }
  std::string line;
  unsigned new_curve_id = 0;
  unsigned old_curve_id = 0;
  PointContainer points;
  unsigned point_id = 0;

  while (std::getline(infile, line)) {
    std::istringstream buffer(line);
    double x, y, z;
    buffer >> new_curve_id >> point_id >> x >> y >> z;
    if (new_curve_id != old_curve_id) {
      if (points.size() > 1) {
        Snake *s = new Snake(points, true, false, image_, external_force_,
                             interpolator_, vector_interpolator_, transform_);
        comparing_snakes1_.push_back(s);
        s->Resample();
        points.clear();
      }
      old_curve_id = new_curve_id;
    }
    PointType p;
    p[0] = x + offset[0];
    p[1] = y + offset[1];
    p[2] = z + offset[2];
    points.push_back(p);
  }
  infile.close();
  if (points.size() > 1) {
    Snake *s = new Snake(points, true, false, image_, external_force_,
                         interpolator_, vector_interpolator_, transform_);
    comparing_snakes1_.push_back(s);
    s->Resample();
    points.clear();
  }
}


void Multisnake::LoadSnakes(const std::string &filename,
                            SnakeContainer &snakes) {
  this->ClearSnakeContainer(snakes);
  std::ifstream infile(filename.c_str());
  if (!infile.is_open()) {
    std::cerr << "LoadSnakes: couldn't open file: " << filename << std::endl;
    return;
  }
  std::string line, name, value;
  PointContainer points;
  bool is_open = true;
  PointContainer junction_points;

  while (std::getline(infile, line)) {
    if (snakes == converged_snakes_ && isalpha(line[0])) {
      std::stringstream converter;
      converter << line;
      converter >> name >> value;
      this->AssignParameters(name, value);
    } else if (line[0] == '#') {
      if (points.size() > 1) {
        Snake *s = new Snake(points, is_open, false, image_, external_force_,
                             interpolator_, vector_interpolator_, transform_);
        snakes.push_back(s);
        s->Resample();
      }
      is_open = (line[1] != '0');
      points.clear();
    } else if (line[0] == '[') {
      this->LoadPoint(line, junction_points);
    } else {
      std::istringstream stream(line);
      double snake_index, point_index, x, y, z;
      stream >> snake_index >> point_index >> x >> y >> z;
      PointType  snake_point;
      snake_point[0] = x;
      snake_point[1] = y;
      snake_point[2] = z;
      points.push_back(snake_point);
    }
  }
  infile.close();

  if (points.size() > 1) {
    Snake *s = new Snake(points, is_open, false, image_, external_force_,
                         interpolator_, vector_interpolator_, transform_);
    snakes.push_back(s);
    s->Resample();
  }
  junctions_.set_junction_points(junction_points);
}

void Multisnake::LoadSnakesSequence(const std::string &filename) {
  std::ifstream infile(filename.c_str());
  if (!infile.is_open()) {
    std::cerr << "LoadSnakesSequence: couldn't open file: " << filename << std::endl;
    return;
  }
  SnakeContainer snakes;

  std::string line, name, value;
  PointContainer points;
  bool is_open = true;
  PointContainer junction_points;
  int frames_done = 0;

  while (std::getline(infile, line)) {
    if (isalpha(line[0])) {
      std::stringstream converter;
      converter << line;
      converter >> name >> value;
      this->AssignParameters(name, value);
    } else if (line[0] == '$') {
      if (frames_done) {
        if (points.size() > 1) {
          Snake *s = new Snake(points, is_open, false, image_, external_force_,
                               interpolator_, vector_interpolator_, transform_);
          snakes.push_back(s);
          s->Resample();
          points.clear();
        }
        converged_snakes_sequence_.push_back(snakes);
        snakes.clear();
        junctions_sequence_.push_back(junction_points);
        junction_points.clear();
      }
      frames_done++;
    } else if (line[0] == '#') {
      if (points.size() > 1) {
        Snake *s = new Snake(points, is_open, false, image_, external_force_,
                             interpolator_, vector_interpolator_, transform_);
        snakes.push_back(s);
        s->Resample();
      }
      is_open = (line[1] != '0');
      points.clear();
    } else if (line[0] == '[') {
      this->LoadPoint(line, junction_points);
    } else {
      std::istringstream stream(line);
      double snake_index, point_index, x, y, z;
      stream >> snake_index >> point_index >> x >> y >> z;
      PointType  snake_point;
      snake_point[0] = x;
      snake_point[1] = y;
      snake_point[2] = z;
      points.push_back(snake_point);
    }
  }
  infile.close();
  // // test code
  // for (int i = 0; i < converged_snakes_sequence_.size(); i++) {
  //   std::cout << "\nFrame #" << i << std::endl;
  //   this->PrintSnakes(converged_snakes_sequence_[i]);
  //   for (int j = 0; j < junctions_sequence_[i].size(); j++) {
  //     std::cout << junctions_sequence_[i][j] << "\t";
  //   }
  // }
}


void Multisnake::LoadJFilamentSnakes(const std::string &filename,
                                     SnakeContainer &snakes) {
  this->ClearSnakeContainer(snakes);
  std::ifstream infile(filename.c_str());
  if (!infile) {
    std::cerr << "Couldn't open file: " << infile << std::endl;
    return;
  }

  std::string line;
  PointContainer points;
  bool is_open = true;
  bool pound  = false;

  while (getline(infile, line)) {
    if (isalpha(line[0])) {
      continue;
    } else if (line[0] == '#') {
      if (points.size() > 1) {
        Snake *s = new Snake(points, is_open, false, image_, external_force_,
                             interpolator_, vector_interpolator_, transform_);
        snakes.push_back(s);
        s->Resample();
      }
      points.clear();
      pound = true;
    } else if (line[0] == '0') {
      if (pound) {
        pound = false;
        continue;
      } else {
        std::istringstream  stream(line);
        double zero, index, x, y, z;
        stream >> zero >> index >> x >> y >> z;
        //std::cout << x << " " << y << " " << z << std::endl;
        PointType  snake_point;
        snake_point[0] = x;
        snake_point[1] = y;
        snake_point[2] = z;
        points.push_back(snake_point);
      }
    }
  }

  infile.close();

  if (points.size() > 1) {
    Snake *s = new Snake(points, is_open, false, image_, external_force_,
                         interpolator_, vector_interpolator_, transform_);
    snakes.push_back(s);
    s->Resample();
  }
}

void Multisnake::LoadPoint(const std::string &s, PointContainer &c) {
  std::istringstream buffer(s);
  char padding;
  double x, y, z;
  buffer >> padding >> x >> padding >> y >> padding >> z >> padding;

  PointType p;
  p[0] = x;
  p[1] = y;
  p[2] = z;
  c.push_back(p);
}

void Multisnake::SaveSnakes(const SnakeContainer &snakes,
                            const std::string &filename) const {
  std::ofstream outfile;
  outfile.open(filename.c_str());
  if (!outfile.is_open()) {
    std::cerr << "SaveSnakes: Couldn't open file: " << filename << std::endl;
    return;
  }

  const unsigned column_width = 16;
  outfile << "image\t" << image_filename_ << std::endl;
  this->WriteParameters(outfile);

  if (snakes.empty()) {
    std::cout << "No snakes to save!" << std::endl;
    return;
  }

  unsigned snake_index = 0;
  // outfile << std::setprecision(12);

  for (SnakeConstIterator it = snakes.begin(); it != snakes.end(); ++it) {
    outfile << "#" << (*it)->open() << std::endl;
    for (unsigned j = 0; j != (*it)->GetSize(); ++j) {
      double intensity = interpolator_->Evaluate((*it)->GetPoint(j));
      outfile << snake_index << std::setw(12) << j
              << std::setw(column_width) << (*it)->GetX(j)
              << std::setw(column_width) << (*it)->GetY(j)
              << std::setw(column_width) << (*it)->GetZ(j)
              << std::setw(20) << intensity << std::endl;
    }
    snake_index++;
  }

  junctions_.PrintJunctionPoints(filename);
  outfile.close();
}

void Multisnake::SaveSnakesSequence(const std::string &filename) const {
  std::ofstream outfile;
  outfile.open(filename.c_str());
  if (!outfile.is_open()) {
    std::cerr << "SaveSnakesSequence: Couldn't open file: " << outfile << std::endl;
    return;
  }

  const unsigned column_width = 16;
  outfile << "image\t" << image_filename_ << std::endl;
  this->WriteParameters(outfile);

  if (converged_snakes_sequence_.empty()) {
    std::cout << "No snakes to save!" << std::endl;
    return;
  }

  for (int i = 0; i < converged_snakes_sequence_.size(); i++) {
    outfile << "$" << i << std::endl;
    unsigned snake_index = 0;
    for (SnakeConstIterator it = converged_snakes_sequence_[i].begin();
         it != converged_snakes_sequence_[i].end(); ++it) {
      outfile << "#" << (*it)->open() << std::endl;
      for (unsigned j = 0; j != (*it)->GetSize(); ++j) {
        double intensity = interpolator_->Evaluate((*it)->GetPoint(j));
        outfile << snake_index << std::setw(12) << j
                << std::setw(column_width) << (*it)->GetX(j)
                << std::setw(column_width) << (*it)->GetY(j)
                << std::setw(column_width) << (*it)->GetZ(j)
                << std::setw(20) << intensity << std::endl;
      }
      snake_index++;
    }
    for (PointContainer::const_iterator it = junctions_sequence_[i].begin();
         it != junctions_sequence_[i].end(); ++it) {
      outfile << *it << std::endl;
    }
  }
  outfile << '$' << std::endl; // mark the end of sequence
  outfile.close();
}

void Multisnake::SaveJFilamentSnakes(const SnakeContainer &snakes,
                                     const std::string &filename) const {
  if (snakes.empty())  {
    std::cerr << "No snakes to save as JFilament snakes!" << std::endl;
    return;
  }

  std::ofstream outfile;
  outfile.open(filename.c_str());
  if (!outfile.is_open()) {
    std::cerr << "Couldn't open file: " << outfile << std::endl;
    return;
  }

  outfile << "gamma\t" << solver_bank_->gamma() << std::endl;
  outfile << "weight\t" << Snake::external_factor() << std::endl;
  outfile << "zresolution\t" << 1.0 << std::endl;
  outfile << "background\t" << background_ << std::endl;
  outfile << "alpha\t" << solver_bank_->alpha() << std::endl;
  outfile << "smoothing\t" << sigma_ << std::endl;
  outfile << "stretch\t" << Snake::stretch_factor() << std::endl;
  outfile << "spacing\t" << Snake::desired_spacing() << std::endl;
  outfile << "beta\t" << solver_bank_->beta() << std::endl;
  outfile << "foreground\t" << foreground_ << std::endl;

  for (SnakeConstIterator it = snakes.begin(); it != snakes.end(); ++it) {
    outfile << "#\n0" << std::endl;

    for (unsigned j = 0; j != (*it)->GetSize(); ++j) {
      outfile << "0\t" << j << "\t";
      outfile << (*it)->GetX(j) << "\t"
              << (*it)->GetY(j) << "\t"
              << (*it)->GetZ(j) << std::endl;
    }
  }
  outfile.close();
}

void Multisnake::PrintSnakes(const SnakeContainer &snakes) const {
  if (snakes.empty()) {
    std::cout << "Snake container is empty!" << std::endl;
    return;
  }
  for (SnakeConstIterator it = snakes.begin(); it != snakes.end(); ++it) {
    (*it)->PrintSelf();
  }
}

void Multisnake::ComputeErrorFromSnakesToComparingSnakes(
    DataContainer &errors) const {
  for (SnakeConstIterator iter = converged_snakes_.begin();
       iter != converged_snakes_.end(); ++iter) {
    for (unsigned i = 0; i < (*iter)->GetSize(); ++i) {
      double dist = this->ComputeShortestDistance((*iter)->GetPoint(i),
                                                  comparing_snakes1_);
      errors.push_back(dist);
    }
  }
}

void Multisnake::ComputeErrorFromComparingSnakesToSnakes(
    DataContainer &errors) const {
  for (SnakeConstIterator iter = comparing_snakes1_.begin();
       iter != comparing_snakes1_.end(); ++iter) {
    for (unsigned i = 0; i < (*iter)->GetSize(); ++i) {
      double dist = this->ComputeShortestDistance((*iter)->GetPoint(i),
                                                  converged_snakes_);
      errors.push_back(dist);
    }
  }
}

double Multisnake::ComputeShortestDistance(
    const PointType &p, const SnakeContainer &snakes) const {
  double min_dist = kPlusInfinity;
  for (SnakeConstIterator iter = snakes.begin();
       iter != snakes.end(); ++iter) {
    for (unsigned i = 0; i < (*iter)->GetSize(); ++i) {
      double dist = p.EuclideanDistanceTo((*iter)->GetPoint(i));
      if (dist < min_dist)
        min_dist = dist;
    }
  }
  return min_dist;
}


void Multisnake::ComputeRadialOrientation(const PointType &center,
                                          double pixel_size,
                                          std::ostream & os) const {
  if (converged_snakes_.empty()) return;

  const unsigned width = 16;
  os << "Image file\t" << image_filename_ << "\n"
          << "Image center\t" << center << "\n"
          << std::setw(width) << "radius (um)"
          << std::setw(width) << "theta" << std::endl;

  for (SnakeConstIterator it = converged_snakes_.begin();
       it != converged_snakes_.end(); ++it) {
    for (unsigned i = 0; i < (*it)->GetSize() - 1; ++i) {
      double r, theta;
      this->ComputeRTheta((*it)->GetPoint(i), (*it)->GetPoint(i+1),
                          center, r, theta);
      os << std::setw(width) << r * pixel_size
              << std::setw(width) << theta << std::endl;
    }
  }
}

void Multisnake::ComputeRTheta(const PointType &point1,
                               const PointType &point2,
                               const PointType &center,
                               double &r, double &theta) const {
  PointType mid;
  mid.SetToMidPoint(point1, point2);
  r = mid.EuclideanDistanceTo(center);

  VectorType vector = point1 - point2;
  VectorType radial = mid - center;
  vector.Normalize();
  radial.Normalize();
  theta = std::acos(vector * radial) * 180 / kPi;
  theta = theta > 90.0 ? (180.0 - theta) : theta;
}

void Multisnake::ComputePointDensityAndIntensity(const PointType &center,
                                                 double max_radius,
                                                 double pixel_size,
                                                 unsigned type,
                                                 std::ostream & os) const {
  if (converged_snakes_.empty()) return;

  const unsigned width = 16;
  unsigned max_r = static_cast<unsigned>(max_radius);

  os << "Image file\t" << image_filename_
     << "\nImage center\t" << center
     << "\nMax radius\t" << max_r
     << "\nType\t" << type << "\n"
     << std::setw(width) << "radius (pixel)"
     << std::setw(width) << "radius (um)"
     << std::setw(width) << "soac density"
     << std::setw(width) << "soac intensity"
     << std::setw(width) << "voxel intensity" << std::endl;

  DataContainer snake_intensities(max_r, 0.0);
  DataContainer voxel_intensities(max_r, 0.0);
  std::vector<unsigned> snaxel_counts(max_r, 0);
  std::vector<unsigned> voxel_counts(max_r, 0);

  // snaxel counts and snake intensities
  for (SnakeConstIterator it = converged_snakes_.begin();
       it != converged_snakes_.end(); ++it) {
    for (unsigned i = 0; i < (*it)->GetSize(); ++i) {
      const PointType &p = (*it)->GetPoint(i);
      unsigned r = static_cast<unsigned>(center.EuclideanDistanceTo(p));
      if (r < max_r) {
        snaxel_counts[r]++;
        snake_intensities[r] += interpolator_->Evaluate(p);
      }
    }
  }

  // voxel counts and voxel intensities
  itk::ImageRegionConstIteratorWithIndex<ImageType>
      it(image_, image_->GetLargestPossibleRegion());
  it.GoToBegin();
  while (!it.IsAtEnd()) {
    ImageType::IndexType index = it.GetIndex();
    PointType p;
    p[0] = index[0];
    p[1] = index[1];
    p[2] = index[2];
    unsigned r = static_cast<unsigned>(center.EuclideanDistanceTo(p));
    if (r < max_r) {
      voxel_counts[r]++;
      voxel_intensities[r] += it.Value();
    }
    ++it;
  }

  // compute density and average intensity
  for (unsigned i = 0; i < max_r; ++i) {
    if (snaxel_counts[i] > 0)
      snake_intensities[i] /= snaxel_counts[i];

    voxel_intensities[i] /= voxel_counts[i];
    double r = (i+1) * pixel_size;
    double density = snaxel_counts[i] / (4 * kPi * r * r);
    os << std::setw(width) << i + 1
       << std::setw(width) << r
       << std::setw(width) << density
       << std::setw(width) << snake_intensities[i]
       << std::setw(width) << voxel_intensities[i] << std::endl;
  }
}

void Multisnake::ComputeCurvature(int coarse_graining, std::ostream &os) const {
  os << "Image file\t" << image_filename_ << "\n"
          << "Coarse graining\t" << coarse_graining << std::endl;

  for (SnakeConstIterator it = converged_snakes_.begin();
       it != converged_snakes_.end(); ++it) {
    const int end_index = (*it)->GetSize() - 2*coarse_graining;
    for (int i = 0; i < end_index; i += coarse_graining) {
      PointType p0 = (*it)->GetPoint(i);
      PointType p1 = (*it)->GetPoint(i + coarse_graining);
      PointType p2 = (*it)->GetPoint(i + 2*coarse_graining);
      VectorType vec1 = p1 - p0;
      vec1.Normalize();
      VectorType vec2 = p2 - p1;
      vec2.Normalize();
      double length = coarse_graining * (*it)->spacing();
      double curvature = (vec1 - vec2).GetNorm() / length;
      os << curvature << std::endl;
    }
  }
}

void Multisnake::ComputeSphericalOrientation(const PointType &center,
                                             double max_r, std::ostream &os) const {
  os << "Polar,Azimuthal" << std::endl;
  for (SnakeConstIterator it = converged_snakes_.begin();
       it != converged_snakes_.end(); ++it) {
    for (unsigned i = 0; i < (*it)->GetSize() - 1; ++i) {
      const PointType &p1 = (*it)->GetPoint(i);
      const PointType &p2 = (*it)->GetPoint(i+1);
      if (this->IsInsideSphere(center, max_r, p1) &&
          this->IsInsideSphere(center, max_r, p2)) {
        double theta, phi;
        this->ComputeThetaPhi(p1 - p2, theta, phi);
        os << theta << "," << phi << std::endl;
      }
    }
  }
}

bool Multisnake::IsInsideSphere(const PointType &center,
                                double r, const PointType p) const {
  return center.EuclideanDistanceTo(p) < r;
}

void Multisnake::ComputeThetaPhi(VectorType vector,
                                 double &theta, double &phi) const {
  // phi is (-pi/2, +pi/2]
  // theta is [0, pi)
  const double r = vector.GetNorm();

  if (std::abs(vector[0]) < kEpsilon && std::abs(vector[1]) < kEpsilon) {
    // x = y = 0
    phi = 0;
    theta = 0;
  } else if (std::abs(vector[0]) < kEpsilon) {
    // x = 0, y != 0
    if (vector[1] < -kEpsilon)
      vector = -vector;

    phi = 90;
    theta = std::acos(vector[2]/r) * 180 / kPi;
  } else {
    // x != 0
    if (vector[0] < -kEpsilon)
      vector = -vector;

    theta = std::acos(vector[2]/r) * 180 / kPi;
    phi = std::atan(vector[1] / vector[0]) * 180 / kPi;
  }
}

void Multisnake::DeleteSnakes(const SnakeSet &snakes) {
  for (SnakeSet::const_iterator it = snakes.begin();
       it != snakes.end(); ++it) {
    SnakeIterator it2 = std::find(converged_snakes_.begin(),
                                  converged_snakes_.end(), *it);
    if (it2 != converged_snakes_.end()) {
      converged_snakes_.erase(it2);
    } else {
      std::cout << "Couldn't locate snake " << *it << " in converged snakes."
                << std::endl;
    }
  }
}

Snake * Multisnake::PopLastInitialSnake() {
  Snake *s = initial_snakes_.back();
  initial_snakes_.pop_back();
  return s;
}


void Multisnake::AddSubsnakesToInitialSnakes(Snake *s) {
  initial_snakes_.insert(initial_snakes_.end(),
                         s->subsnakes().begin(), s->subsnakes().end());
}

double Multisnake::ComputeGroundTruthFValue(const DataContainer &snrs,
                                            double threshold,
                                            double penalizer) const {
  return this->ComputeFValue(snrs, threshold, penalizer) /
      comparing_snakes1_.size();
}

double Multisnake::ComputeResultSnakesFValue(const DataContainer &snrs,
                                             double threshold,
                                             double penalizer) const {
  return this->ComputeFValue(snrs, threshold, penalizer) /
      converged_snakes_.size();
}

double Multisnake::ComputeFValue(const DataContainer &snrs,
                                 double threshold, double penalizer) const {
  if (snrs.empty()) return 0.0;
  unsigned low_snr_npoints = 0;
  for (DataContainer::const_iterator it = snrs.begin(); it != snrs.end();
       ++it) {
    if (*it < threshold) low_snr_npoints++;
  }
  return penalizer * low_snr_npoints - snrs.size();
}

void Multisnake::PrintGroundTruthLocalSNRValues(int radial_near,
                                                int radial_far) const {
  DataContainer snrs;
  this->ComputeLocalSNRs(comparing_snakes1_, radial_near, radial_far, snrs);
  if (snrs.empty()) return;
  std::cout << "========= Local SNR Info =========" << std::endl;
  std::cout << "Min: " << Minimum(snrs) << "\tMax: " << Maximum(snrs)
            << std::endl;
  double mean_snr = Mean(snrs);
  std::cout << "Mean: " << mean_snr << "\t Median: " << Median(snrs)
            << "\t Std: " << StandardDeviation(snrs, mean_snr) << std::endl;
}

void Multisnake::ComputeGroundTruthLocalSNRs(int radial_near, int radial_far,
                                             DataContainer &snrs) const {
  this->ComputeLocalSNRs(comparing_snakes1_, radial_near, radial_far, snrs);
}

void Multisnake::ComputeResultSnakesLocalSNRs(int radial_near, int radial_far,
                                              DataContainer &snrs) const {
  this->ComputeLocalSNRs(converged_snakes_, radial_near, radial_far, snrs);
}

void Multisnake::ComputeLocalSNRs(const SnakeContainer &snakes,
                                  int radial_near, int radial_far,
                                  DataContainer &snrs) const {
  if (snakes.empty()) return;
  for (SnakeConstIterator it = snakes.begin(); it != snakes.end(); it++) {
    for (unsigned i = 0; i < (*it)->GetSize(); i++) {
      double local_snr = 0.0;
      bool local_bg_defined = (*it)->ComputeLocalSNRAtIndex(
          i, radial_near, radial_far, local_snr);

      if (local_bg_defined)
        snrs.push_back(local_snr);
      // else
      //   std::cerr << "Local background undefined!" << std::endl;
    }
  }
}

void Multisnake::ComputeResultSnakesVertexErrorHausdorffDistance(
    double &vertex_error, double &hausdorff) const {
  if (converged_snakes_.empty() || comparing_snakes1_.empty()) return;

  DataContainer errors1, errors2;

  this->ComputeErrorFromSnakesToComparingSnakes(errors1);
  this->ComputeErrorFromComparingSnakesToSnakes(errors2);
  vertex_error = (Mean(errors1) + Mean(errors2))/2;
  // std::cout << "ve: " << vertex_error << std::endl;
  double max_error1 = Maximum(errors1);
  double max_error2 = Maximum(errors2);
  hausdorff = max_error1 > max_error2 ? max_error1 : max_error2;
  // std::cout << "hausdorff: " << hausdorff << std::endl;
}

void Multisnake::GenerateSyntheticImageShotNoise(unsigned foreground, unsigned background,
                                                 unsigned offset, double scaling,
                                                 const std::string &filename) const {
  if (!image_ || comparing_snakes1_.empty()) return;

  FloatImageType::Pointer img = FloatImageType::New();
  img->SetRegions(image_->GetLargestPossibleRegion());
  img->Allocate();
  img->FillBuffer(background);

  // Assign centerline intensities
  for (SnakeConstIterator it = comparing_snakes1_.begin();
       it != comparing_snakes1_.end(); ++it) {
    for (unsigned i = 0; i < (*it)->GetSize(); ++i) {
      FloatImageType::IndexType index;
      img->TransformPhysicalPointToIndex((*it)->GetPoint(i), index);
      // img->SetPixel(index, img->GetPixel(index) + foreground);
      img->SetPixel(index, foreground);
    }
  }

  // apply PSF
  double variance[3] = {3.0, 3.0, 3*2.88*2.88};
  typedef itk::DiscreteGaussianImageFilter<FloatImageType, FloatImageType> GaussianFilterType;
  GaussianFilterType::Pointer gaussian = GaussianFilterType::New();
  gaussian->SetInput(img);
  gaussian->SetVariance(variance);

  // rescale the intensity back to foreground
  typedef itk::RescaleIntensityImageFilter<FloatImageType, FloatImageType> RescalerType;
  RescalerType::Pointer rescaler = RescalerType::New();
  rescaler->SetInput(gaussian->GetOutput());
  rescaler->SetOutputMinimum(background);
  rescaler->SetOutputMaximum(foreground);
  rescaler->Update();
  img = rescaler->GetOutput();

  typedef itk::ImageDuplicator<FloatImageType> DuplicatorType;
  DuplicatorType::Pointer duplicator = DuplicatorType::New();
  duplicator->SetInputImage(img);
  duplicator->Update();
  FloatImageType::Pointer clean_img = duplicator->GetOutput();

  // apply shot noise
  typedef itk::ImageRegionIterator<FloatImageType> IteratorType;
  IteratorType iter(img, img->GetLargestPossibleRegion());
  std::mt19937 generator(2003);
  for (iter.GoToBegin(); !iter.IsAtEnd(); ++iter) {
    std::poisson_distribution<> distribution(iter.Get() * scaling);
    double intensity = distribution(generator) / scaling + offset;
    double assigned_intensity = intensity > std::numeric_limits<unsigned short>::max()?
        std::numeric_limits<unsigned short>::max() : intensity;
    iter.Set(assigned_intensity);
  }

  // compute image snr
  // typedef itk::SquareImageFilter <FloatImageType, FloatImageType> SquareImageFilterType;
  // SquareImageFilterType::Pointer square = SquareImageFilterType::New();
  // square->SetInput(clean_img);
  // square->Update();
  // typedef itk::SquaredDifferenceImageFilter<FloatImageType,
  //                                           FloatImageType,
  //                                           FloatImageType> SquaredDifferenceImageFilterType;
  // SquaredDifferenceImageFilterType::Pointer squared_differ = SquaredDifferenceImageFilterType::New();
  // squared_differ->SetInput1(img);
  // squared_differ->SetInput2(clean_img);
  // squared_differ->Update();

  typedef itk::StatisticsImageFilter<FloatImageType> StatisticsImageFilterType;
  StatisticsImageFilterType::Pointer stat1 = StatisticsImageFilterType::New();
  stat1->SetInput(clean_img);
  stat1->Update();
  // StatisticsImageFilterType::Pointer stat2 = StatisticsImageFilterType::New();
  // stat2->SetInput(squared_differ->GetOutput());
  // stat2->Update();
  double mean_intensity = stat1->GetMean();
  std::cout << "Mean: " << mean_intensity << std::endl;
  // double snr = std::sqrt(mean_intensity * scaling);
  double snr = (mean_intensity - background) * std::sqrt(scaling / background);
  std::cout << "SNR: " << snr << std::endl;



  // compute the difference image
  // typedef itk::AbsoluteValueDifferenceImageFilter <FloatImageType,
  //                                                  FloatImageType,
  //                                                  FloatImageType> DifferenceImageFilterType;

  // DifferenceImageFilterType::Pointer differ = DifferenceImageFilterType::New();
  // differ->SetInput1(img);
  // differ->SetInput2(clean_img);
  // differ->Update();
  // FloatImageType::Pointer diff_img = differ->GetOutput();

  typedef itk::CastImageFilter<FloatImageType, ImageType> CasterType;
  CasterType::Pointer caster = CasterType::New();
  caster->SetInput(img);

  typedef itk::ImageFileWriter<ImageType> WriterType;
  WriterType::Pointer writer = WriterType::New();
  writer->SetFileName(filename);
  writer->SetInput(caster->GetOutput());
  try {
    writer->Update();
  } catch(itk::ExceptionObject &e) {
    std::cerr << "Multisnake::GenerateSyntheticImageShotNoise: Exception caught!\n"
              << e << std::endl;
  }

  CasterType::Pointer caster2 = CasterType::New();
  // caster2->SetInput(diff_img);
  // const std::string label("-diff");
  caster2->SetInput(clean_img);
  const std::string label("-clean");
  std::string diff_filename = filename;
  diff_filename.insert(diff_filename.begin() + diff_filename.find_last_of('.'),
                       label.begin(), label.end());
  WriterType::Pointer writer2 = WriterType::New();
  writer2->SetFileName(diff_filename);
  writer2->SetInput(caster2->GetOutput());
  writer2->Update();
}


void Multisnake::GenerateSyntheticTamara(double foreground, double background,
                                         const char *filename) const {
  if (comparing_snakes1_.empty()) return;
  FloatImageType::Pointer img = FloatImageType::New();
  FloatImageType::IndexType start;
  start.Fill(0);
  FloatImageType::SizeType size;
  size[0] = 80;
  size[1] = 150;
  size[2] = 80;
  FloatImageType::RegionType region;
  region.SetSize(size);
  region.SetIndex(start);
  img->SetRegions(region);
  img->Allocate();
  img->FillBuffer(0.0);

  for (SnakeConstIterator it = comparing_snakes1_.begin();
       it != comparing_snakes1_.end(); ++it) {
    for (unsigned i = 0; i < (*it)->GetSize(); ++i) {
      FloatImageType::IndexType index;
      img->TransformPhysicalPointToIndex((*it)->GetPoint(i), index);
      img->SetPixel(index, foreground);
    }
  }

  double variance[3] = {3.0, 3.0, 12.0};
  typedef itk::DiscreteGaussianImageFilter<FloatImageType,
                                           FloatImageType> GaussianFilterType;
  GaussianFilterType::Pointer gaussian = GaussianFilterType::New();
  gaussian->SetInput(img);
  gaussian->SetVariance(variance);

  // rescale the intensity back to foreground
  typedef itk::RescaleIntensityImageFilter<FloatImageType, FloatImageType> RescalerType;
  RescalerType::Pointer rescaler = RescalerType::New();
  rescaler->SetInput(gaussian->GetOutput());
  rescaler->SetOutputMinimum(0.0);
  rescaler->SetOutputMaximum(foreground);
  rescaler->Update();
  img = rescaler->GetOutput();

  double sigma = 1.0;
  typedef itk::ImageRegionIterator<FloatImageType> IteratorType;
  IteratorType iter(img, img->GetLargestPossibleRegion());

  typedef itk::Statistics::NormalVariateGenerator GeneratorType;
  GeneratorType::Pointer generator = GeneratorType::New();
  generator->Initialize(2003);

  for (iter.GoToBegin(); !iter.IsAtEnd(); ++iter) {
    iter.Value() += background + sigma * generator->GetVariate();
  }

  typedef itk::CastImageFilter<FloatImageType, ImageType> CasterType;
  CasterType::Pointer caster = CasterType::New();
  caster->SetInput(img);

  typedef itk::ImageFileWriter<ImageType> WriterType;
  WriterType::Pointer writer = WriterType::New();
  writer->SetFileName(filename);
  writer->SetInput(caster->GetOutput());
  try {
    writer->Update();
  } catch(itk::ExceptionObject &e) {
    std::cerr << "Exception caught!\n" << e << std::endl;
  }
}

void Multisnake::GenerateSyntheticImage(unsigned foreground,
                                        unsigned background,
                                        double sigma,
                                        const std::string &filename)
    const {
  if (!image_ || comparing_snakes1_.empty()) return;

  FloatImageType::Pointer img = FloatImageType::New();
  img->SetRegions(image_->GetLargestPossibleRegion());
  img->Allocate();
  if (sigma < kEpsilon) {
    // std::cout << "generating clean image..." << std::endl;
    img->FillBuffer(background);
  } else {
    img->FillBuffer(0.0);
  }

  // Assign centerline intensities
  if (foreground) {
    for (SnakeConstIterator it = comparing_snakes1_.begin();
         it != comparing_snakes1_.end(); ++it) {
      for (unsigned i = 0; i < (*it)->GetSize(); ++i) {
        FloatImageType::IndexType index;
        img->TransformPhysicalPointToIndex((*it)->GetPoint(i), index);
        if (sigma < kEpsilon)
          img->SetPixel(index, img->GetPixel(index) + foreground);
        else
          img->SetPixel(index, foreground);
      }
    }

    // apply PSF
    double variance[3] = {3.0, 3.0, 3*2.88*2.88};
    typedef itk::DiscreteGaussianImageFilter<
      FloatImageType, FloatImageType> GaussianFilterType;
    GaussianFilterType::Pointer gaussian = GaussianFilterType::New();
    gaussian->SetInput(img);
    gaussian->SetVariance(variance);

    // rescale the intensity back to foreground
    typedef itk::RescaleIntensityImageFilter<
      FloatImageType, FloatImageType> RescalerType;
    RescalerType::Pointer rescaler = RescalerType::New();
    rescaler->SetInput(gaussian->GetOutput());
    if (sigma < kEpsilon) {
      rescaler->SetOutputMinimum(background);
      rescaler->SetOutputMaximum(foreground + background);
    } else {
      rescaler->SetOutputMinimum(0.0);
      rescaler->SetOutputMaximum(foreground);
    }
    rescaler->Update();
    img = rescaler->GetOutput();
  }

  // add Gaussian noise with u=background and std=sigma
  if (sigma > 0.0) {
    typedef itk::ImageRegionIterator<FloatImageType> IteratorType;
    IteratorType iter(img, img->GetLargestPossibleRegion());

    typedef itk::Statistics::NormalVariateGenerator GeneratorType;
    GeneratorType::Pointer generator = GeneratorType::New();
    generator->Initialize(2003);

    for (iter.GoToBegin(); !iter.IsAtEnd(); ++iter) {
      iter.Value() += background + sigma * generator->GetVariate();
    }
  }

  typedef itk::CastImageFilter<FloatImageType, ImageType> CasterType;
  CasterType::Pointer caster = CasterType::New();
  caster->SetInput(img);

  typedef itk::ImageFileWriter<ImageType> WriterType;
  WriterType::Pointer writer = WriterType::New();
  writer->SetFileName(filename);
  writer->SetInput(caster->GetOutput());
  try {
    writer->Update();
  } catch(itk::ExceptionObject &e) {
    std::cerr << "Snake::SaveImageUShort: Exception caught!\n"
              << e << std::endl;
  }
  return;
}

void Multisnake::GenerateSyntheticRealImage(
    double ratio, double sigma, const std::string &filename) const {
  if (!image_ || comparing_snakes1_.empty()) return;

  const double background = 230.0;
  FloatImageType::Pointer img = FloatImageType::New();
  img->SetRegions(image_->GetLargestPossibleRegion());
  img->Allocate();
  if (sigma <= 0.0)
    img->FillBuffer(background);
  else
    img->FillBuffer(0.0);

  // Assign centerline intensities
  double max_intensity = .0;
  for (SnakeConstIterator it = comparing_snakes1_.begin();
       it != comparing_snakes1_.end(); ++it) {
    for (unsigned i = 0; i < (*it)->GetSize(); ++i) {
      double intensity = interpolator_->Evaluate((*it)->GetPoint(i));
      FloatImageType::IndexType index;
      img->TransformPhysicalPointToIndex((*it)->GetPoint(i), index);
      img->SetPixel(index, img->GetPixel(index) + intensity);
      if (intensity > max_intensity)
        max_intensity = intensity;
    }
  }

  // apply PSF
  double variance[3] = {3.0, 3.0, 3*2.88*2.88};
  typedef itk::DiscreteGaussianImageFilter<
    FloatImageType, FloatImageType> GaussianFilterType;
  GaussianFilterType::Pointer gaussian = GaussianFilterType::New();
  gaussian->SetInput(img);
  gaussian->SetVariance(variance);

  const double foreground = ratio * (max_intensity - background);
  // rescale the intensity back to foreground
  typedef itk::RescaleIntensityImageFilter<
    FloatImageType, FloatImageType> RescalerType;
  RescalerType::Pointer rescaler = RescalerType::New();
  rescaler->SetInput(gaussian->GetOutput());
  if (sigma <= 0.0) {
    rescaler->SetOutputMinimum(background);
    rescaler->SetOutputMaximum(background + foreground);
  } else {
    rescaler->SetOutputMinimum(0.0);
    rescaler->SetOutputMaximum(foreground);
  }
  rescaler->Update();
  img = rescaler->GetOutput();

  // add Gaussian noise with u=background and std=sigma
  if (sigma > 0.0) {
    typedef itk::ImageRegionIterator<FloatImageType> IteratorType;
    IteratorType iter(img, img->GetLargestPossibleRegion());

    typedef itk::Statistics::NormalVariateGenerator GeneratorType;
    GeneratorType::Pointer generator = GeneratorType::New();
    generator->Initialize(2003);

    for (iter.GoToBegin(); !iter.IsAtEnd(); ++iter) {
      iter.Value() += background + sigma * generator->GetVariate();
    }
  }

  typedef itk::CastImageFilter<FloatImageType, ImageType> CasterType;
  CasterType::Pointer caster = CasterType::New();
  caster->SetInput(img);

  typedef itk::ImageFileWriter<ImageType> WriterType;
  WriterType::Pointer writer = WriterType::New();
  writer->SetFileName(filename);
  writer->SetInput(caster->GetOutput());
  try {
    writer->Update();
  } catch(itk::ExceptionObject &e) {
    std::cerr << "Snake::SaveImageUShort: Exception caught!\n"
              << e << std::endl;
  }
}

void Multisnake::set_intensity_scaling(double scale) {
  if (scale > kEpsilon) {
    intensity_scaling_ = scale;
  } else if (image_) {
    intensity_scaling_ = 1.0 / this->GetMaxImageIntensity();
  }
}

ImageType::PixelType Multisnake::GetMaxImageIntensity() const {
  if (!image_) return 0;
  typedef itk::MinimumMaximumImageCalculator<ImageType> FilterType;
  FilterType::Pointer filter = FilterType::New();
  filter->SetImage(image_);
  filter->ComputeMaximum();
  return filter->GetMaximum();
}

double Multisnake::ComputeDropletMeanIntensity(PointType center, double radius) const {
  DataContainer intensities;
  typedef itk::ImageRegionConstIteratorWithIndex<ImageType> IteratorType;
  assert(image_);
  IteratorType it(image_, image_->GetLargestPossibleRegion());
  for (it.GoToBegin(); !it.IsAtEnd(); ++it) {
    PointType p;
    image_->TransformIndexToPhysicalPoint(it.GetIndex(), p);
    if (p.EuclideanDistanceTo(center) < radius)
      intensities.push_back(it.Get());
  }
  return Mean(intensities);
}

void Multisnake::ComputeSOACPointsFraction(const PointType &center,
                                           double radius,
                                           unsigned binning,
                                           DataContainer &fractions) const {
  assert(!converged_snakes_.empty());
  std::vector<unsigned> counts(binning, 0);
  std::vector<double> ranges(binning, 0.0);
  double radius_step = radius / binning;
  for (int i = 0; i < binning; i++) {
    ranges[i] = radius_step * (i + 1);
  }
  ranges.back() = kPlusInfinity;  // last bucket is for >= 0.9r
  unsigned number_of_soac_points = this->GetNumberOfSOACPoints(converged_snakes_);
  for (SnakeConstIterator it = converged_snakes_.begin();
       it != converged_snakes_.end(); ++it) {
    for (unsigned j = 0; j != (*it)->GetSize(); ++j) {
      double distance_to_center = center.EuclideanDistanceTo((*it)->GetPoint(j));
      std::vector<double>::iterator position_it = std::upper_bound(
          ranges.begin(), ranges.end(), distance_to_center);
      std::vector<double>::difference_type n = position_it - ranges.begin();
      assert(n >= 0);
      counts[n]++;
    }
  }

  for (unsigned i = 0; i < binning; i++) {
    double percentage = static_cast<double>(counts[i]) / number_of_soac_points * 100;
    fractions.push_back(percentage);
  }
}

unsigned Multisnake::GetNumberOfSOACPoints(const SnakeContainer &snakes) const {
  unsigned count = 0;
  for (SnakeConstIterator it = snakes.begin(); it != snakes.end(); ++it) {
    count += (*it)->GetSize();
  }
  return count;
}

} // namespace soax
