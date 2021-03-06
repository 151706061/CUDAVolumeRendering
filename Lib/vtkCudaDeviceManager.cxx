/** @file vtkCUDADeviceManager.cxx
*
*  @brief A singleton class to manage cards and stream interleaving
*
*  @author John Stuart Haberl Baxter (Dr. Peter's Lab at Robarts Research Institute)
*  @note First documented on June 12, 2012
*
*/

#include "cuda_runtime_api.h"

// CUDA includes
#include "vtkCUDADeviceManager.h"
#include "vtkCUDAObject.h"

// VTK includes
#include <vtkMutexLock.h>
#include <vtkObjectFactory.h>

// STD includes
#include <set>

vtkCUDADeviceManager* vtkCUDADeviceManager::singletonManager = 0;

vtkCUDADeviceManager* vtkCUDADeviceManager::Singleton(){
  if( !singletonManager )
    singletonManager = new vtkCUDADeviceManager();
  return singletonManager;
  }

vtkCUDADeviceManager::vtkCUDADeviceManager(){

  //create the locks
  this->regularLock = 0;
  // \tbd is something missing here ?
  //int n = this->GetNumberOfDevices();

  }

vtkCUDADeviceManager::~vtkCUDADeviceManager(){

  //define a list to collect the used device ID's in
  std::set<int> devicesInUse;
  this->regularLock->Lock();

  //synchronize and end all streams
  for( std::map<cudaStream_t*,int>::iterator it = this->StreamToDeviceMap.begin();
    it != this->StreamToDeviceMap.end(); it++ ){
      this->SynchronizeStream( (*it).first );
      cudaStreamDestroy( *(it->first) );
      devicesInUse.insert( it->second );
    }

  //decommission the devices
  for( std::set<int>::iterator it = devicesInUse.begin();
    it != devicesInUse.end(); it++ ){
      cudaSetDevice( *it );
      cudaDeviceReset( );
    }

  //clean up variables
  this->StreamToDeviceMap.clear();
  this->StreamToObjectMap.clear();
  this->ObjectToDeviceMap.clear();
  this->regularLock->Unlock();
  this->regularLock->Delete();

  }

int vtkCUDADeviceManager::GetNumberOfDevices(){

  int numberOfDevices = 0;
  cudaError_t result = cudaGetDeviceCount (&numberOfDevices);

  if( result != 0 ){
    vtkErrorMacro(<<"Catostrophic CUDA error - cannot count number of devices.");
    return -1;
    }
  return numberOfDevices;

  }

bool vtkCUDADeviceManager::GetDevice(vtkCUDAObject* caller, int device){
  if( device < 0 || device >= this->GetNumberOfDevices() ){
    vtkErrorMacro(<<"Invalid device identifier.");
    return true;
    }

  //tell if we should allocate the lock
  if( this->ObjectToDeviceMap.empty() )
    regularLock = vtkMutexLock::New();

  //remove that part of the mapping
  this->regularLock->Lock();
  this->ObjectToDeviceMap.insert( std::pair<vtkCUDAObject*,int>(caller, device) );
  this->regularLock->Unlock();
  return false;
  }

bool vtkCUDADeviceManager::ReturnDevice(vtkCUDAObject* caller, int device){
  this->regularLock->Lock();

  //find if that is a valid mapping
  bool found = false;
  bool emptyDevice = true;
  std::multimap<vtkCUDAObject*,int>::iterator it = this->ObjectToDeviceMap.begin();
  std::multimap<vtkCUDAObject*,int>::iterator eraseIt = this->ObjectToDeviceMap.begin();
  for( ; it != this->ObjectToDeviceMap.end(); it++ ){
    if( it->first == caller && it->second == device ){
      found = true;
      eraseIt = it;
      }else if(it->second == device){
        emptyDevice = false;
      }
    }
  if( !found ){
    vtkErrorMacro(<<"Could not locate supplied caller-device pair.");
    this->regularLock->Unlock();
    return true;
    }

  //also remove streams associated with this caller and this device
  std::set<cudaStream_t*> streamsToReturn;
  std::multimap<cudaStream_t*,vtkCUDAObject*>::iterator it2 = this->StreamToObjectMap.begin();
  for( ; it2 != this->StreamToObjectMap.end(); it2++ ){
    if( this->StreamToDeviceMap[(it2->first)] == device ){
      // \tbd is something missing here ?
      //cudaStream_t* tempStreamPointer = it2->first;
      }
    }
  for( std::set<cudaStream_t*>::iterator it = streamsToReturn.begin();
    it != streamsToReturn.end(); it++ )
    this->ReturnStream(caller, *it, device );

  //remove that part of the mapping
  this->ObjectToDeviceMap.erase(eraseIt);
  if( emptyDevice ){
    int oldDevice = 0;
    cudaGetDevice( &oldDevice );
    if( device != oldDevice ) cudaSetDevice( device );
    cudaDeviceReset();
    if( device != oldDevice ) cudaSetDevice( oldDevice );
    }
  this->regularLock->Unlock();

  //tell if we should deallocate the lock
  if( this->ObjectToDeviceMap.empty() )
    regularLock->UnRegister(this);

  return false;
  }

bool vtkCUDADeviceManager::GetStream(vtkCUDAObject* caller, cudaStream_t** stream, int device){

  //check device identifier for consistency
  if( device < 0 || device >= this->GetNumberOfDevices() ){
    vtkErrorMacro(<<"Invalid device identifier.");
    return true;
    }

  //if stream is provided, check for stream-device consistancy
  this->regularLock->Lock();
  if( *stream && this->StreamToDeviceMap.count(*stream) == 1 && 
    this->StreamToDeviceMap[(*stream)] != device ){
      this->regularLock->Unlock();
      vtkErrorMacro(<<"Stream already assigned to particular device.");
      return true;
    }

  //find if this call is redundant
  bool found = false;
  std::multimap<cudaStream_t*,vtkCUDAObject*>::iterator it = this->StreamToObjectMap.begin();
  for( ; it != this->StreamToObjectMap.end(); it++ )
    if( it->first == *stream && it->second == caller )
      found = true;
  if( found ){
    this->regularLock->Unlock();
    return false;
    }

  //create the new stream and mapping
  cudaSetDevice(device);
  if( *stream == 0 ){
    cudaStream_t* temp = new cudaStream_t();
    cudaStreamCreate( temp );
    *stream = temp;
    }
  this->StreamToDeviceMap.insert( std::pair<cudaStream_t*,int>(*stream,device) );
  this->StreamToObjectMap.insert( std::pair<cudaStream_t*,vtkCUDAObject*>(*stream,caller) );
  this->regularLock->Unlock();

  return false;

  }

bool vtkCUDADeviceManager::ReturnStream(vtkCUDAObject* caller, cudaStream_t* stream, int device){
  this->regularLock->Lock();

  //find if that is a valid three-tuple (stream, object, device)
  bool found = false;
  std::multimap<cudaStream_t*,vtkCUDAObject*>::iterator it = this->StreamToObjectMap.begin();
  for( ; it != this->StreamToObjectMap.end(); it++ ){
    if( it->first == stream && it->second == caller &&
      this->StreamToDeviceMap[(it->first)] == device ){
        found = true;
        break;
      }
    }
  if( !found ){
    vtkErrorMacro(<<"Could not locate supplied caller-device pair.");
    this->regularLock->Unlock();
    return true;
    }

  //remove that part of the mapping
  this->StreamToObjectMap.erase(it);
  if( this->StreamToObjectMap.count(stream) == 0 )
    this->StreamToDeviceMap.erase( stream );
  this->regularLock->Unlock();
  return false;

  }

bool vtkCUDADeviceManager::SynchronizeStream( cudaStream_t* stream ){

  //find mapped result and device
  this->regularLock->Lock();
  if( this->StreamToDeviceMap.count(stream) != 1 ){
    vtkErrorMacro(<<"Cannot synchronize unused stream.");
    this->regularLock->Unlock();
    return true;
    }
  int device = this->StreamToDeviceMap[(stream)];
  this->regularLock->Unlock();

  //synchronize the stream and return the success value
  int oldDevice = -1;
  cudaGetDevice( &oldDevice );
  cudaSetDevice( device );
  cudaStreamSynchronize( *stream );
  cudaSetDevice( oldDevice );
  return cudaGetLastError() != cudaSuccess;

  }

bool vtkCUDADeviceManager::ReserveGPU( cudaStream_t* stream ){

  //find mapped result and device
  this->regularLock->Lock();
  if( this->StreamToDeviceMap.count(stream) != 1 ){
    vtkErrorMacro(<<"Cannot synchronize unused stream.");
    this->regularLock->Unlock();
    return true;
    }
  int device = this->StreamToDeviceMap[(stream)];
  this->regularLock->Unlock();

  //synchronize the stream and return the success value
  cudaSetDevice( device );
  return cudaGetLastError() != cudaSuccess;

  }

int vtkCUDADeviceManager::QueryDeviceForObject( vtkCUDAObject* object ){
  this->regularLock->Lock();
  int device = -1;
  if( this->ObjectToDeviceMap.count(object) == 1 )
    device = this->ObjectToDeviceMap.find(object)->second;
  else
    vtkErrorMacro(<<"No unique mapping exists.");
  this->regularLock->Unlock();
  return device;
  }

int vtkCUDADeviceManager::QueryDeviceForStream( cudaStream_t* stream ){
  this->regularLock->Lock();
  int device = -1;
  if( this->StreamToDeviceMap.count(stream) == 1 )
    device = this->StreamToDeviceMap[(stream)];
  else
    vtkErrorMacro(<<"No mapping exists.");
  this->regularLock->Unlock();
  return device;
  }

void vtkCUDADeviceManager::DestroyEmptyStream( cudaStream_t* stream ){

  this->SynchronizeStream( stream );

  bool found = false;
  std::map<cudaStream_t*,int>::iterator it = this->StreamToDeviceMap.begin();
  for( ; it != this->StreamToDeviceMap.end(); it++ ){
    if( it->first == stream ){
      found = true;
      break;
      }
    }
  if( !found ){
    vtkErrorMacro(<<"Could not locate supplied caller-device pair.");
    return;
    }
  this->StreamToDeviceMap.erase(it);

  }
