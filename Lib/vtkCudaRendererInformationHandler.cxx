/** @file vtkCUDARendererInformationHandler.cxx
*
*  @brief An internal class for vtkCUDAVolumeMapper which manages information regarding the renderer, camera, shading model and other objects
*
*  @author John Stuart Haberl Baxter (Dr. Peter's Lab at Robarts Research Institute)
*  @note First documented on March 29, 2011
*
*/

#include "vtkCUDARendererInformationHandler.h"
#include "CUDA_vtkCUDAVolumeMapper_renderAlgo.h"
#include "vector_functions.h"

// VTK includes
#include <vtkCamera.h>
#include <vtkObjectFactory.h>
#include <vtkMatrix4x4.h>
#include <vtkPlane.h>
#include <vtkPlaneCollection.h>
#include <vtkRenderer.h>
#include <vtkRenderWindow.h>

// STD includes
#include <vector>

vtkStandardNewMacro(vtkCUDARendererInformationHandler);

vtkCUDARendererInformationHandler::vtkCUDARendererInformationHandler()
  {
  this->Renderer = 0;
  this->RendererInfo.actualResolution.x = this->RendererInfo.actualResolution.y = 0;
  this->RendererInfo.NumberOfClippingPlanes = 0;

  SetGradientShadingConstants(0.605f);

  this->ZBuffer = 0;

  this->clipModified = 0;


  }

void vtkCUDARendererInformationHandler::Deinitialize(int withData)
  {
  this->ReserveGPU();
  CUDA_vtkCUDAVolumeMapper_renderAlgo_unloadZBuffer(this->GetStream());
  }

void vtkCUDARendererInformationHandler::Reinitialize(int withData)
  {
  //nothing to do - ZBuffer handled when run
  }

vtkRenderer* vtkCUDARendererInformationHandler::GetRenderer()
  {
  return this->Renderer;
  }

void vtkCUDARendererInformationHandler::SetRenderer(vtkRenderer* renderer)
  {
  this->Renderer = renderer;
  this->Update();
  }

void vtkCUDARendererInformationHandler::SetGradientShadingConstants(float darkness)
  {
  if(darkness >= 0.0f && darkness <= 1.0f ){
    this->RendererInfo.gradShadeScale = darkness;
    this->RendererInfo.gradShadeShift = 1.0 - darkness;
    }
  }

void vtkCUDARendererInformationHandler::Update()
  {
  if (this->Renderer != 0)
    {
    // Renderplane Update.
    int *size = this->Renderer->GetSize();
    this->RendererInfo.actualResolution.x = size[0];
    this->RendererInfo.actualResolution.y = size[1];
    }
  }

void vtkCUDARendererInformationHandler::SetViewToVoxelsMatrix(vtkMatrix4x4* matrix)
  {
  //load the original table
  for(int i = 0; i < 4; i++){
    for(int j = 0; j < 4; j++){
      this->RendererInfo.ViewToVoxelsMatrix[i*4+j] = matrix->GetElement(i,j);
      }
    }

  //compute the obtimizations to measure the view via the x,y position of the pixel divided by the resolution
  this->RendererInfo.ViewToVoxelsMatrix[3] += this->RendererInfo.ViewToVoxelsMatrix[0] - this->RendererInfo.ViewToVoxelsMatrix[1];
  this->RendererInfo.ViewToVoxelsMatrix[7] += this->RendererInfo.ViewToVoxelsMatrix[4] - this->RendererInfo.ViewToVoxelsMatrix[5];
  this->RendererInfo.ViewToVoxelsMatrix[11] += this->RendererInfo.ViewToVoxelsMatrix[8] - this->RendererInfo.ViewToVoxelsMatrix[9];
  this->RendererInfo.ViewToVoxelsMatrix[15] += this->RendererInfo.ViewToVoxelsMatrix[12] - this->RendererInfo.ViewToVoxelsMatrix[13];

  this->RendererInfo.ViewToVoxelsMatrix[0] *= -2.0f;
  this->RendererInfo.ViewToVoxelsMatrix[4] *= -2.0f;
  this->RendererInfo.ViewToVoxelsMatrix[8] *= -2.0f;
  this->RendererInfo.ViewToVoxelsMatrix[12] *= -2.0f;

  this->RendererInfo.ViewToVoxelsMatrix[1] *= 2.0f;
  this->RendererInfo.ViewToVoxelsMatrix[5] *= 2.0f;
  this->RendererInfo.ViewToVoxelsMatrix[9] *= 2.0f;
  this->RendererInfo.ViewToVoxelsMatrix[13] *= 2.0f;

  }

void vtkCUDARendererInformationHandler::SetWorldToVoxelsMatrix(vtkMatrix4x4* matrix)
  {
  this->clipModified = 0;
  for(int i = 0; i < 4; i++){
    for(int j = 0; j < 4; j++){
      this->WorldToVoxelsMatrix[i*4+j] = matrix->GetElement(i,j);
      }
    }
  }

void vtkCUDARendererInformationHandler::SetVoxelsToWorldMatrix(vtkMatrix4x4* matrix)
  {
  this->clipModified = 0;
  for(int i = 0; i < 4; i++){
    for(int j = 0; j < 4; j++){
      this->VoxelsToWorldMatrix[i*4+j] = matrix->GetElement(i,j);
      }
    }
  }

void vtkCUDARendererInformationHandler::SetClippingPlanes(vtkPlaneCollection* planes)
  {

  //see if we need to refigure the clipping planes
  if(!planes || planes->GetMTime() < this->clipModified)
    {
    return;
    }
  else
    {
    this->clipModified = planes->GetMTime();
    this->FigurePlanes(planes, this->RendererInfo.ClippingPlanes,
      &(this->RendererInfo.NumberOfClippingPlanes) );
    }

  }

void vtkCUDARendererInformationHandler::LoadZBuffer()
  {

  //image origin in pixels
  int x1 = this->Renderer->GetOrigin()[0];
  int y1 = this->Renderer->GetOrigin()[1];
  int x2 = x1 + this->RendererInfo.actualResolution.x - 1;
  int y2 = y1 + this->RendererInfo.actualResolution.y - 1;

  //remove old zBuffer and get a new one
  if(this->ZBuffer) delete this->ZBuffer;
  this->ZBuffer = this->Renderer->GetRenderWindow()->GetZbufferData(x1,y1,x2,y2);
  this->ReserveGPU();
  CUDA_vtkCUDAVolumeMapper_renderAlgo_loadZBuffer(this->ZBuffer, this->RendererInfo.actualResolution.x, this->RendererInfo.actualResolution.y, this->GetStream() );

  }

void vtkCUDARendererInformationHandler::FigurePlanes(vtkPlaneCollection* planes, float* planesArray, int* numberOfPlanes){

  //figure out the number of planes
  *numberOfPlanes = 0;
  if(planes) *numberOfPlanes = planes->GetNumberOfItems();

  //if we don't have a good number of planes, act as if we have none
  if( *numberOfPlanes != 6 )
    {
    *numberOfPlanes = 0;
    return;
    }

  double worldNormal[3];
  double worldOrigin[3];
  double volumeOrigin[4];

  //load the planes into the local buffer and then into the CUDA buffer, providing the required pointer at the end
  for(int i = 0; i < *numberOfPlanes; i++)
    {
    vtkPlane* onePlane = planes->GetItem(i);

    onePlane->GetNormal(worldNormal);
    onePlane->GetOrigin(worldOrigin);

    planesArray[4*i] = worldNormal[0]*VoxelsToWorldMatrix[0]  + worldNormal[1]*VoxelsToWorldMatrix[4]  + worldNormal[2]*VoxelsToWorldMatrix[8];
    planesArray[4*i+1] = worldNormal[0]*VoxelsToWorldMatrix[1]  + worldNormal[1]*VoxelsToWorldMatrix[5]  + worldNormal[2]*VoxelsToWorldMatrix[9];
    planesArray[4*i+2] = worldNormal[0]*VoxelsToWorldMatrix[2]  + worldNormal[1]*VoxelsToWorldMatrix[6]  + worldNormal[2]*VoxelsToWorldMatrix[10];

    volumeOrigin[0] = worldOrigin[0]*WorldToVoxelsMatrix[0]  + worldOrigin[1]*WorldToVoxelsMatrix[1]  + worldOrigin[2]*WorldToVoxelsMatrix[2]  + WorldToVoxelsMatrix[3];
    volumeOrigin[1] = worldOrigin[0]*WorldToVoxelsMatrix[4]  + worldOrigin[1]*WorldToVoxelsMatrix[5]  + worldOrigin[2]*WorldToVoxelsMatrix[6]  + WorldToVoxelsMatrix[7];
    volumeOrigin[2] = worldOrigin[0]*WorldToVoxelsMatrix[8]  + worldOrigin[1]*WorldToVoxelsMatrix[9]  + worldOrigin[2]*WorldToVoxelsMatrix[10] + WorldToVoxelsMatrix[11];
    volumeOrigin[3] = worldOrigin[0]*WorldToVoxelsMatrix[12] + worldOrigin[1]*WorldToVoxelsMatrix[13] + worldOrigin[2]*WorldToVoxelsMatrix[14] + WorldToVoxelsMatrix[15];
    if ( volumeOrigin[3] != 1.0 ) { volumeOrigin[0] /= volumeOrigin[3]; volumeOrigin[1] /= volumeOrigin[3]; volumeOrigin[2] /= volumeOrigin[3]; }

    planesArray[4*i+3] = -(planesArray[4*i]*volumeOrigin[0] + planesArray[4*i+1]*volumeOrigin[1] + planesArray[4*i+2]*volumeOrigin[2]);
    }

  }