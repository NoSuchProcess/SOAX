#include "viewer.h"
#include "QVTKWidget.h"
#include "itkImageToVTKImageFilter.h"
#include "vtkImageCast.h"
#include "vtkImagePlaneWidget.h"
#include "vtkRenderer.h"
#include "vtkRenderWindow.h"
#include "vtkCamera.h"
#include "vtkVolume.h"
#include "vtkOrientationMarkerWidget.h"
#include "vtkCubeAxesActor.h"
#include "vtkCornerAnnotation.h"
#include "vtkVolumeProperty.h"
#include "vtkVolumeRayCastMapper.h"
#include "vtkVolumeRayCastMIPFunction.h"
#include "vtkPiecewiseFunction.h"
#include "vtkColorTransferFunction.h"
#include "vtkAxesActor.h"
#include "vtkCaptionActor2D.h"
#include "vtkTextProperty.h"
#include "vtkProperty.h"
#include "vtkPolyDataMapper.h"
#include "vtkOutlineSource.h"

#include "itkStatisticsImageFilter.h"


namespace soax {

double Viewer::kWhite[3] = {1.0, 1.0, 1.0};
double Viewer::kGray[3] = {0.8, 0.8, 0.8};
double Viewer::kRed[3] = {1.0, 0.0, 0.0};
double Viewer::kMagenta[3] = {1.0, 0.0, 1.0};
double Viewer::kYellow[3] = {1.0, 1.0, 0.0};
double Viewer::kGreen[3] = {0.0, 1.0, 0.0};
double Viewer::kCyan[3] = {0.0, 1.0, 1.0};
double Viewer::kBlue[3] = {0.0, 0.0, 1.0};

Viewer::Viewer() {
  qvtk_ = new QVTKWidget;
  renderer_ = vtkSmartPointer<vtkRenderer>::New();
  qvtk_->GetRenderWindow()->AddRenderer(renderer_);
  camera_ = vtkSmartPointer<vtkCamera>::New();
  renderer_->SetActiveCamera(camera_);

  for (unsigned i = 0; i < kDimension; i++) {
    slice_planes_[i] = vtkSmartPointer<vtkImagePlaneWidget>::New();
    slice_planes_[i]->SetResliceInterpolateToCubic();
    slice_planes_[i]->DisplayTextOn();
    slice_planes_[i]->SetInteractor(qvtk_->GetInteractor());
    slice_planes_[i]->PlaceWidget();
    slice_planes_[i]->SetSliceIndex(0);
    slice_planes_[i]->SetMarginSizeX(0);
    slice_planes_[i]->SetMarginSizeY(0);
    slice_planes_[i]->SetRightButtonAction(
        vtkImagePlaneWidget::VTK_SLICE_MOTION_ACTION);
    slice_planes_[i]->SetMiddleButtonAction(
        vtkImagePlaneWidget::VTK_WINDOW_LEVEL_ACTION);
  }

  volume_ = vtkSmartPointer<vtkVolume>::New();
  orientation_marker_ = vtkSmartPointer<vtkOrientationMarkerWidget>::New();
  corner_text_ = vtkSmartPointer<vtkCornerAnnotation>::New();
  cube_axes_ = vtkSmartPointer<vtkCubeAxesActor>::New();
  bounding_box_ = vtkSmartPointer<vtkActor>::New();
}

Viewer::~Viewer() {
  delete qvtk_;
}

void Viewer::SetupImage(ImageType::Pointer image) {
  typedef itk::ImageToVTKImageFilter<ImageType>  ConnectorType;
  ConnectorType::Pointer connector = ConnectorType::New();
  connector->SetInput(image);
  connector->Update();
  vtkSmartPointer<vtkImageCast> caster = vtkSmartPointer<vtkImageCast>::New();
  caster->SetInputData(connector->GetOutput());
  caster->SetOutputScalarTypeToUnsignedShort();
  caster->Update();

  typedef itk::StatisticsImageFilter<ImageType> FilterType;
  FilterType::Pointer filter = FilterType::New();
  filter->SetInput(image);
  filter->Update();
  double min_intensity = filter->GetMinimum();
  double max_intensity = filter->GetMaximum();

  this->SetupSlicePlanes(caster->GetOutput(), min_intensity,
                         max_intensity);
  this->SetupMIPRendering(caster->GetOutput(), min_intensity,
                          max_intensity);
  this->SetupOrientationMarker();
  this->SetupUpperLeftCornerText(min_intensity, max_intensity);
  this->SetupBoundingBox();
  this->SetupCubeAxes(caster->GetOutput());
}

void Viewer::SetupSlicePlanes(vtkImageData *data, double min_intensity,
                              double max_intensity) {
  double window = max_intensity - min_intensity;
  double level = min_intensity + window/2;

  for (unsigned i = 0; i < kDimension; ++i) {
    slice_planes_[i]->SetInputData(data);
    slice_planes_[i]->SetPlaneOrientation(i);
    slice_planes_[i]->UpdatePlacement();
    slice_planes_[i]->SetWindowLevel(window, level);
  }
}

void Viewer::ToggleSlicePlanes(bool state) {
  if (state) {
    for (unsigned i = 0; i < kDimension; ++i) {
      slice_planes_[i]->On();
    }
    renderer_->ResetCamera();
  } else {
    for (unsigned i = 0; i < kDimension; ++i) {
      slice_planes_[i]->Off();
    }
  }
  this->Render();
}

void Viewer::SetupMIPRendering(vtkImageData *data, double min_intensity,
                               double max_intensity) {
  vtkSmartPointer<vtkPiecewiseFunction> opacity_function =
      vtkSmartPointer<vtkPiecewiseFunction>::New();
  double starting_transparent_intensity = min_intensity +
      0.05 * (max_intensity - min_intensity);
  opacity_function->AddPoint(starting_transparent_intensity, 0.0);
  opacity_function->AddPoint(max_intensity, 1.0);

  vtkSmartPointer<vtkColorTransferFunction> color_function =
      vtkSmartPointer<vtkColorTransferFunction>::New();
  color_function->SetColorSpaceToRGB();
  // color is all white
  color_function->AddRGBPoint(0, 1, 1, 1);
  color_function->AddRGBPoint(255, 1, 1, 1);

  vtkSmartPointer<vtkVolumeProperty> mip_volume_property =
      vtkSmartPointer<vtkVolumeProperty>::New();
  mip_volume_property->SetScalarOpacity(opacity_function);
  mip_volume_property->SetColor(color_function);
  mip_volume_property->SetInterpolationTypeToLinear();
  volume_->SetProperty(mip_volume_property);

  vtkSmartPointer<vtkVolumeRayCastMapper> mapper =
      vtkSmartPointer<vtkVolumeRayCastMapper>::New();
  mapper->SetInputData(data);
  vtkSmartPointer<vtkVolumeRayCastMIPFunction> mip_function =
      vtkSmartPointer<vtkVolumeRayCastMIPFunction>::New();
  mapper->SetVolumeRayCastFunction(mip_function);
  volume_->SetMapper(mapper);
}

void Viewer::ToggleMIPRendering(bool state) {
  if (state) {
    renderer_->AddViewProp(volume_);
    renderer_->ResetCamera();
  } else {
    renderer_->RemoveViewProp(volume_);
  }
  this->Render();
}

void Viewer::SetupOrientationMarker() {
  vtkSmartPointer<vtkAxesActor> axes = vtkSmartPointer<vtkAxesActor>::New();
  const int font_size = 14;
  axes->GetXAxisCaptionActor2D()->GetCaptionTextProperty()->
      SetFontSize(font_size);
  axes->GetYAxisCaptionActor2D()->GetCaptionTextProperty()->
      SetFontSize(font_size);
  axes->GetZAxisCaptionActor2D()->GetCaptionTextProperty()->
      SetFontSize(font_size);
  // orientation_marker_->SetOutlineColor(0, 0, 1);
  orientation_marker_->SetOrientationMarker(axes);
  orientation_marker_->SetInteractor(qvtk_->GetInteractor());
  orientation_marker_->SetViewport(0, 0, 0.3, 0.3);
  // orientation_marker_->SetEnabled(true);
  // orientation_marker_->InteractiveOn();
  // axes->SetXAxisLabelText("hello");
}

void Viewer::ToggleOrientationMarker(bool state) {
  orientation_marker_->SetEnabled(state);
  if (state)
    orientation_marker_->InteractiveOff();
  this->Render();
}

void Viewer::SetupUpperLeftCornerText(unsigned min_intensity,
                                      unsigned max_intensity) {
  std::ostringstream buffer;
  buffer << "Intensity Range: [" << min_intensity << ", "
         << max_intensity << "]";
  corner_text_->SetText(2, buffer.str().c_str());
}

void Viewer::ToggleCornerText(bool state) {
  if (state)
    renderer_->AddViewProp(corner_text_);
  else
    renderer_->RemoveViewProp(corner_text_);
  this->Render();
}

void Viewer::SetupBoundingBox() {
  vtkSmartPointer<vtkOutlineSource> outline =
      vtkSmartPointer<vtkOutlineSource>::New();
  vtkSmartPointer<vtkPolyDataMapper> outline_mapper =
      vtkSmartPointer<vtkPolyDataMapper>::New();
  outline_mapper->SetInputConnection(outline->GetOutputPort());
  outline->SetBounds(volume_->GetBounds());
  bounding_box_->PickableOff();
  bounding_box_->DragableOff();
  bounding_box_->SetMapper(outline_mapper);
  bounding_box_->GetProperty()->SetLineWidth(2.0);
  bounding_box_->GetProperty()->SetColor(kRed);
  bounding_box_->GetProperty()->SetAmbient(1.0);
  bounding_box_->GetProperty()->SetDiffuse(0.0);
}

void Viewer::ToggleBoundingBox(bool state) {
  if (state)
    renderer_->AddActor(bounding_box_);
  else
    renderer_->RemoveActor(bounding_box_);
  this->Render();
}

void Viewer::SetupCubeAxes(vtkImageData *image) {
  cube_axes_->SetBounds(image->GetBounds());
  cube_axes_->SetCamera(camera_);
  // cube_axes_->SetFlyModeToStaticTriad();
  // std::cout << cube_axes_->GetCornerOffset() << std::endl;
  // cube_axes_->SetCornerOffset(0.1);

  // cube_axes_->XAxisTickVisibilityOff();
  // cube_axes_->XAxisLabelVisibilityOff();
  cube_axes_->GetTitleTextProperty(0)->SetColor(kRed);
  cube_axes_->GetLabelTextProperty(0)->SetColor(kRed);
  cube_axes_->GetTitleTextProperty(1)->SetColor(kGreen);
  cube_axes_->GetLabelTextProperty(1)->SetColor(kGreen);
  cube_axes_->GetTitleTextProperty(2)->SetColor(kBlue);
  cube_axes_->GetLabelTextProperty(2)->SetColor(kBlue);
  // cube_axes_->GetTitleTextProperty(0)->SetFontSize(40);
  // std::cout << cube_axes_->GetTitleTextProperty(0)->GetFontSize() << std::endl;
  // std::cout << cube_axes_->GetXAxisLabelVisibility() << std::endl;
  vtkSmartPointer<vtkProperty> axes_property =
      vtkSmartPointer<vtkProperty>::New();
  axes_property->SetLineWidth(2.0);
  axes_property->SetColor(kRed);
  cube_axes_->SetXAxesLinesProperty(axes_property);
  axes_property->SetColor(kGreen);
  cube_axes_->SetYAxesLinesProperty(axes_property);
  axes_property->SetColor(kBlue);
  cube_axes_->SetZAxesLinesProperty(axes_property);
}

void Viewer::ToggleCubeAxes(bool state) {
  if (state)
    renderer_->AddActor(cube_axes_);
  else
    renderer_->RemoveActor(cube_axes_);
  this->Render();
}

void Viewer::Render() {
  qvtk_->GetRenderWindow()->Render();
}

} // namespace soax