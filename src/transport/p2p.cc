/*************************************************************************
 * Copyright (c) 2016-2022, NVIDIA CORPORATION. All rights reserved.
 *
 * See LICENSE.txt for license information
 ************************************************************************/

#include <vector>

#include "comm.h"
#include "graph.h"
#include "utils.h"
#include "shm.h"

#define MERGE_MEMCPY  1

struct ncclP2pBuff {
  void* directPtr;
  cudaIpcMemHandle_t devIpc;
};

struct p2pConnectInfo {
  int rank;
  int read;
  int graphId;
  int channelId;
  struct ncclP2pBuff p2pBuff;
  // Use by CE memcpy
  char shmName[7];
  int shmSize;
};
static_assert(sizeof(struct p2pConnectInfo) <= CONNECT_SIZE, "p2pConnectInfo is too large");

struct p2pShm {
  struct ncclSendMem sendMem;
  struct ncclRecvMem recvMem;
};
struct p2pMemcpyEvent {
  cudaEvent_t ev;
  int cnt;
  int flag;
  struct p2pMemcpyEvent* tracking;
  std::vector<struct p2pMemcpyEvent*> trackers;
};
struct p2pProxyInfo {
  // Shared memory between proxy and receiving GPU
  struct p2pShm* shm;
  struct p2pShm* devShm;
  char shmName[7];
  int shmSize;

  // Intermediate step for sender
  struct ncclRecvMem* ceRecvMem;
  char* ceDevBuff;
  int* offsets;

  // Receiver buffer
  char* recvFifo;

  // Used by progress only
  uint64_t step;
  cudaStream_t stream;
  struct p2pMemcpyEvent events[NCCL_STEPS];
};
static_assert(sizeof(p2pConnectInfo) <= CONNECT_SIZE, "P2P Connect info is too large");

struct p2pSendResources {
  struct ncclSendMem* devMem;
  void* sendMemIpc;
  void* recvMemIpc;
  struct p2pProxyInfo proxyInfo;
};

struct p2pRecvResources {
  struct ncclRecvMem* devMem;
  void* sendMemIpc;
  void* recvMemIpc;
  struct p2pShm* shm;
  struct p2pShm* devShm;
  int shmSize;
};

#include <sys/types.h>

/* Convert a PCI busId string into a local cudaDev device index (cf. CUDA_VISIBLE_DEVICES) */
static int busIdToCudaDev(int64_t busId) {
  int ndev;
  if (cudaGetDeviceCount(&ndev) != cudaSuccess)
    return -1;
  for (int i = 0; i < ndev; i++) {
    char devBusIdStr[NVML_DEVICE_PCI_BUS_ID_BUFFER_SIZE];
    if (cudaDeviceGetPCIBusId(devBusIdStr, NVML_DEVICE_PCI_BUS_ID_BUFFER_SIZE, i) != cudaSuccess)
      return -1;
    int64_t devBusId;
    NCCLCHECK(busIdToInt64(devBusIdStr, &devBusId));
    if (busId == devBusId) return i;
  }
  // BusId was not found in our locally visible CUDA devices
  return -1;
}

NCCL_PARAM(P2pUseCudaMemcpy, "P2P_USE_CUDA_MEMCPY", 1);
static int useMemcpy = 0;
static void initCeOperation();

/* Determine if two peers can communicate through p2p */
ncclResult_t p2pCanConnect(int* ret, struct ncclTopoSystem* topo, struct ncclTopoGraph* graph, struct ncclPeerInfo* info1, struct ncclPeerInfo* info2) {
  initCeOperation();

  // Rule out different nodes / isolated containers
  if (info1->hostHash != info2->hostHash || info1->shmDev != info2->shmDev) {
    *ret = 0;
    return ncclSuccess;
  }

  // Check topology / p2p level.
  int intermediateRank;
  NCCLCHECK(ncclTopoCheckP2p(topo, info1->busId, info2->busId, ret, NULL, &intermediateRank));
  if (*ret == 0) return ncclSuccess;
  if (intermediateRank != -1) {
    if (useMemcpy) *ret = 0;
    return ncclSuccess;
  }

  // Convert the peer's busId into a local cudaDev index (cf. CUDA_VISIBLE_DEVICES)
  int cudaDev1 = busIdToCudaDev(info1->busId);
  int cudaDev2 = busIdToCudaDev(info2->busId);
  if (cudaDev1 == -1 || cudaDev2 == -1) {
#if CUDART_VERSION >= 10010
    // CUDA 10.1 and later can use P2P with invisible devices.
    return ncclSuccess;
#else
    // Peer's CUDA device is not visible in this process : we can't communicate with it.
    *ret = 0;
    return ncclSuccess;
#endif
  }

  // Check that CUDA can do P2P
  int p2p;
  if (cudaDeviceCanAccessPeer(&p2p, cudaDev1, cudaDev2) != cudaSuccess) {
    INFO(NCCL_INIT|NCCL_P2P,"peer query failed between dev %d(=%lx) and dev %d(=%lx)",
         cudaDev1, info1->busId, cudaDev2, info2->busId);
    *ret = 0;
    return ncclSuccess;
  }

  if (p2p != 0) {
    // Cached result of the legacyIPC detection
    static int legacyIPC = -1;
    if (legacyIPC >= 0) {
      *ret = legacyIPC;
      return ncclSuccess;
    }
    // Check that legacy IPC support is available (WSL WAR)
    char *dummy;
    cudaIpcMemHandle_t ipc;
    NCCLCHECK(ncclCudaCalloc(&dummy, CUDA_IPC_MIN));
    if (cudaIpcGetMemHandle(&ipc, dummy) != cudaSuccess) {
      INFO(NCCL_INIT|NCCL_P2P,"Legacy IPC not supported");
      *ret = 0;
    }
    CUDACHECK(cudaFree(dummy));
    legacyIPC = *ret;
    return ncclSuccess;
  }

  if (p2p == 0) {
    INFO(NCCL_INIT|NCCL_P2P,"Could not enable P2P between dev %d(=%lx) and dev %d(=%lx)",
         cudaDev1, info1->busId, cudaDev2, info2->busId);
    *ret = 0;
    return ncclSuccess;
  }
  return ncclSuccess;
}

#define TRACE_DUMP_IPC(DEVIPC)                                                             \
  do {                                                                                     \
    unsigned long *devIpc = (unsigned long *) (DEVIPC);                                    \
    TRACE(P2P,"IPC: %016lx %016lx %016lx %016lx", devIpc[0], devIpc[1], devIpc[2], devIpc[3]); \
    TRACE(P2P,"IPC: %016lx %016lx %016lx %016lx", devIpc[4], devIpc[5], devIpc[6], devIpc[7]); \
  } while (0)


// Setting this to non zero causes P2P to use Reads rather than Writes
NCCL_PARAM(P2pReadEnable, "P2P_READ_ENABLE", 0);
NCCL_PARAM(P2pDirectDisable, "P2P_DIRECT_DISABLE", 0);

static ncclResult_t p2pMemcpyEventCreate(struct p2pMemcpyEvent *e)
{
  if (cudaEventCreateWithFlags(&e->ev, cudaEventDisableTiming) != cudaSuccess) return ncclInternalError;
  e->flag = 1;
  e->tracking = nullptr;
  e->trackers.clear();
  return ncclSuccess;
}
static ncclResult_t p2pMemcpyEventDestroy(struct p2pMemcpyEvent *e)
{
  if (cudaEventDestroy(e->ev) != cudaSuccess) return ncclInternalError;
  e->trackers.clear();
  return ncclSuccess;
}
static ncclResult_t p2pMemcpyEventRecord(struct p2pMemcpyEvent *e, cudaStream_t s)
{
  if (e->flag != 1) {
    WARN("Overwriting an unresolved event record.");
  }
  if (!e->trackers.empty()) {
    for (std::vector<struct p2pMemcpyEvent *>::iterator it = e->trackers.begin();
          it != e->trackers.end(); ++it) {
      (*it)->flag = e->flag;
    }
    e->trackers.clear();
  }
  if (cudaEventRecord(e->ev, s) != cudaSuccess) return ncclInternalError;
  e->flag = 0;
  e->tracking = nullptr;
  return ncclSuccess;
}
static ncclResult_t p2pMemcpyEventTrack(struct p2pMemcpyEvent *e, struct p2pMemcpyEvent *t)
{
  // Tracker should not track another tracker
  if (t->tracking) return ncclInternalError;
  if (!e->trackers.empty()) return ncclInternalError;
  e->tracking = t;
  e->flag = t->flag;
  if (!t->flag) t->trackers.emplace_back(e);
  return ncclSuccess;
}
static ncclResult_t p2pMemcpyEventUntrack(struct p2pMemcpyEvent *e)
{
  if (e->tracking) {
    if (!e->flag) {
      for (std::vector<struct p2pMemcpyEvent *>::iterator it = e->tracking->trackers.begin();
           it != e->tracking->trackers.end(); ++it) {
        if (*it == e) {
          e->tracking->trackers.erase(it);
          break;
        }
      }
      e->flag = 1;
    }
    e->tracking = nullptr;
  }
  return ncclSuccess;
}
static ncclResult_t p2pMemcpyEventQuery(struct p2pMemcpyEvent *e, int *result)
{
  if (e->flag == 1) {
    *result = 1;
    return ncclSuccess;
  }
  if (e->tracking) {
    if (p2pMemcpyEventQuery(e->tracking, result) != ncclSuccess) return ncclInternalError;
    return ncclSuccess;
  }
  cudaError_t err = cudaEventQuery(e->ev);
  if (err == cudaSuccess) {
    e->flag = 1;
    *result = 1;
    for (std::vector<struct p2pMemcpyEvent *>::iterator it = e->trackers.begin();
          it != e->trackers.end(); ++it) {
      (*it)->flag = 1;
    }
    e->trackers.clear();
    return ncclSuccess;
  } else if (err == cudaErrorNotReady) {
    *result = 0;
    return ncclSuccess;
  }
  return ncclInternalError;
}

static ncclResult_t p2pGetInfo(struct ncclTopoSystem* topo, struct ncclPeerInfo* info1, struct ncclPeerInfo* info2, int* read, int* intermediateRank) {
  int p2p;
  // Queries the topology to see if the GPUs are Ampere and
  // connected via NVLink, if so we enable P2P Read by default
  NCCLCHECK(ncclTopoCheckP2p(topo, info1->busId, info2->busId, &p2p, read, intermediateRank));

  int readEnable = ncclParamP2pReadEnable();
  if (readEnable != -2) *read = readEnable;
  return ncclSuccess;
}

static ncclResult_t p2pMap(struct ncclPeerInfo* myInfo, struct ncclPeerInfo* peerInfo, struct ncclP2pBuff* p2pBuff, void** devMem, void** ipcPtr) {
  if (myInfo->pidHash == peerInfo->pidHash) {
    if (peerInfo->cudaDev != myInfo->cudaDev) {
      // Enable P2P access
      cudaError_t err = cudaDeviceEnablePeerAccess(peerInfo->cudaDev, 0);
      if (err == cudaErrorPeerAccessAlreadyEnabled) {
        cudaGetLastError();
      } else if (err != cudaSuccess) {
        WARN("failed to peer with device %d(=%lx): %d %s",
            peerInfo->cudaDev, peerInfo->busId, err, cudaGetErrorString(err));
        return ncclInternalError;
      }
    }
    *devMem = p2pBuff->directPtr;
    *ipcPtr = NULL;
  } else {
    CUDACHECK(cudaIpcOpenMemHandle(devMem, p2pBuff->devIpc, cudaIpcMemLazyEnablePeerAccess));
    *ipcPtr = *devMem;
  }
  return ncclSuccess;
}

/* Send: Create and return connect structures for this peer to connect to me */
ncclResult_t p2pSendSetup(struct ncclComm* comm, struct ncclTopoGraph* graph, struct ncclPeerInfo* myInfo, struct ncclPeerInfo* peerInfo,
    struct ncclConnect* connectInfo, struct ncclConnector* send, int channelId, int connIndex) {
  struct p2pSendResources* resources;
  NCCLCHECK(ncclCalloc(&resources, 1));
  send->transportResources = resources;
  int useRead, intermediateRank;
  NCCLCHECK(p2pGetInfo(comm->topo, myInfo, peerInfo, &useRead, &intermediateRank));
  if (useMemcpy) useRead = 0;

  static_assert(sizeof(struct p2pConnectInfo) <= sizeof(struct ncclConnect), "p2p Connect Info is too big");
  struct p2pConnectInfo* info = (struct p2pConnectInfo*)connectInfo;
  info->read = useRead;
  info->graphId = graph->id;
  info->channelId = channelId;
  // For CollNet, use write for scatter-reduce (conn 1), read for broadcast-gather (conn 0)
  if (graph && connIndex == 1) info->read = 0;
  const char* useReadStr = info->read ? "/read" : "";

  int sendSize = sizeof(struct ncclSendMem);
  // For P2P Read the SIMPLE buffer is tagged on the end of the ncclSendMem structure
  if (info->read) sendSize += send->comm->buffSizes[NCCL_PROTO_SIMPLE];
  ALIGN_SIZE(sendSize, CUDA_IPC_MIN);

  if (intermediateRank == -1) {
    info->rank = myInfo->rank;
    if (myInfo->pidHash == peerInfo->pidHash && useMemcpy == 0) {
      if (ncclParamP2pDirectDisable() == 0) send->conn.direct |= info->read ? NCCL_DIRECT_READ : NCCL_DIRECT_WRITE;
      INFO(NCCL_INIT|NCCL_P2P, "Channel %02d : %d[%lx] -> %d[%lx] via P2P/direct pointer%s",
          channelId, myInfo->rank, myInfo->busId, peerInfo->rank, peerInfo->busId, useReadStr);
    } else {
      send->conn.direct |= info->read ? NCCL_IPC_READ : NCCL_IPC_WRITE;
      INFO(NCCL_INIT|NCCL_P2P,"Channel %02d : %d[%lx] -> %d[%lx] via P2P/IPC%s%s",
          channelId, myInfo->rank, myInfo->busId, peerInfo->rank, peerInfo->busId, useReadStr, useMemcpy ? "/CE" : "");
    }
  } else {
    info->rank = intermediateRank;
    INFO(NCCL_INIT|NCCL_P2P, "Channel %02d : %d[%lx] -> %d[%lx] via P2P/indirect/%d[%lx]%s",
        channelId, myInfo->rank, myInfo->busId, peerInfo->rank, peerInfo->busId, intermediateRank,
	comm->peerInfo[intermediateRank].busId, useReadStr);
  }

  NCCLCHECK(ncclProxyConnect(comm, TRANSPORT_P2P, 1, info->rank, &send->proxyConn));
  if (useMemcpy) {
    NCCLCHECK(ncclProxyCall(&send->proxyConn, ncclProxyMsgSetup, NULL, 0, &resources->proxyInfo, sizeof(struct p2pProxyInfo)));
    info->shmSize = resources->proxyInfo.shmSize;
    memcpy(info->shmName, resources->proxyInfo.shmName, sizeof(info->shmName));
  } else {
    NCCLCHECK(ncclProxyCall(&send->proxyConn, ncclProxyMsgSetup, &sendSize, sizeof(int), &info->p2pBuff, sizeof(struct ncclP2pBuff)));
    NCCLCHECK(p2pMap(myInfo, comm->peerInfo+info->rank, &info->p2pBuff, (void**)&resources->devMem, &resources->sendMemIpc));
  }

  return ncclSuccess;
}

/* Create and return connect structures for this peer to connect to me */
ncclResult_t p2pRecvSetup(struct ncclComm* comm, struct ncclTopoGraph* graph, struct ncclPeerInfo* myInfo, struct ncclPeerInfo* peerInfo,
    struct ncclConnect* connectInfo, struct ncclConnector * recv, int channelId, int connIndex) {
  struct p2pRecvResources* resources;
  NCCLCHECK(ncclCalloc(&resources, 1));
  recv->transportResources = resources;
  int useRead, intermediateRank;
  NCCLCHECK(p2pGetInfo(comm->topo, myInfo, peerInfo, &useRead, &intermediateRank));

  static_assert(sizeof(struct p2pConnectInfo) <= sizeof(struct ncclConnect), "p2p Connect Info is too big");
  struct p2pConnectInfo* info = (struct p2pConnectInfo*)connectInfo;
  info->read = useRead;
  info->graphId = graph->id;
  info->channelId = channelId;
  // For CollNet, use write for scatter-reduce (conn 1), read for broadcast-gather (conn 0)
  if (graph && connIndex == 1) info->read = 0;

  int recvSize = sizeof(struct ncclRecvMem);
  // For P2P Read the SIMPLE buffer is tagged on the end of the ncclSendMem structure
  for (int p=0; p<NCCL_NUM_PROTOCOLS; p++) if (!(info->read && p == NCCL_PROTO_SIMPLE)) recvSize += recv->comm->buffSizes[p];
  ALIGN_SIZE(recvSize, CUDA_IPC_MIN);

  if (intermediateRank == -1) {
    info->rank = myInfo->rank;
    if (myInfo->pidHash == peerInfo->pidHash && useMemcpy == 0) {
      if (ncclParamP2pDirectDisable() == 0) recv->conn.direct |= info->read ? NCCL_DIRECT_READ : NCCL_DIRECT_WRITE;
    } else {
      recv->conn.direct |= info->read ? NCCL_IPC_READ : NCCL_IPC_WRITE;
    }
  } else {
    info->rank = intermediateRank;
  }

  NCCLCHECK(ncclProxyConnect(comm, TRANSPORT_P2P, 0, info->rank, &recv->proxyConn));
  NCCLCHECK(ncclProxyCall(&recv->proxyConn, ncclProxyMsgSetup, &recvSize, sizeof(int), &info->p2pBuff, sizeof(struct ncclP2pBuff)));

  NCCLCHECK(p2pMap(myInfo, comm->peerInfo+info->rank, &info->p2pBuff, (void**)&resources->devMem, &resources->recvMemIpc));
  return ncclSuccess;
}

/* Connect/Send to this peer */
static ncclResult_t p2pSendConnect(struct ncclComm* comm, struct ncclConnect* connectInfo, int nranks, int rank, struct ncclConnector* send) {
  struct p2pSendResources* resources = (struct p2pSendResources*)send->transportResources;
  struct ncclRecvMem* remDevMem;
  struct p2pConnectInfo* info = (struct p2pConnectInfo*)connectInfo;

  NCCLCHECK(p2pMap(comm->peerInfo+rank, comm->peerInfo+info->rank, &info->p2pBuff, (void**)&remDevMem, &resources->recvMemIpc));

  char* buff = (char*)(remDevMem+1);
  for (int p=0; p<NCCL_NUM_PROTOCOLS; p++) {
    if (info->read && p == NCCL_PROTO_SIMPLE) {
      /* For P2P Read the SIMPLE buffer is local (ncclSendMem) */
      if (resources->devMem == NULL) return ncclInternalError; // We should not use read + memcpy
      send->conn.buffs[p] = (char*)(resources->devMem+1);
    } else {
      send->conn.buffs[p] = buff;
      buff += send->comm->buffSizes[p];
    }
  }

  if (useMemcpy) {
    send->conn.tail = &resources->proxyInfo.ceRecvMem->tail;
    send->conn.sizesFifo = resources->proxyInfo.ceRecvMem->sizesFifo;
    send->conn.head = &resources->proxyInfo.devShm->sendMem.head;
    // Send SIMPLE buff to proxy, and replace it by local buffer
    NCCLCHECK(ncclProxyCall(&send->proxyConn, ncclProxyMsgConnect, &send->conn.buffs[NCCL_PROTO_SIMPLE], sizeof(void*), NULL, 0));
    send->conn.buffs[NCCL_PROTO_SIMPLE] = resources->proxyInfo.ceDevBuff;
#if (MERGE_MEMCPY == 1)
    int stepSize = send->comm->buffSizes[NCCL_PROTO_SIMPLE]/NCCL_STEPS;
    for (int i=0; i<NCCL_STEPS; ++i) {
      resources->proxyInfo.offsets[i] =
          stepSize*(i*comm->nChannels + info->channelId + info->graphId*NCCL_STEPS*comm->nChannels);
    }
    NCCLCHECK(ncclCudaCalloc(&send->conn.offsFifo, NCCL_STEPS));
    NCCLCHECK(ncclCudaMemcpy(send->conn.offsFifo, resources->proxyInfo.offsets, NCCL_STEPS));
#endif
  } else {
    send->conn.tail = &remDevMem->tail;
    send->conn.head = &resources->devMem->head;
    send->conn.ptrExchange = &resources->devMem->ptrExchange;
    send->conn.redOpArgExchange = resources->devMem->redOpArgExchange;
  }
  return ncclSuccess;
}

/* Connect/Recv from this peer */
ncclResult_t p2pRecvConnect(struct ncclComm* comm, struct ncclConnect* connectInfo, int nranks, int rank, struct ncclConnector* recv) {
  struct p2pRecvResources* resources = (struct p2pRecvResources*)recv->transportResources;
  struct p2pConnectInfo* info = (struct p2pConnectInfo*)connectInfo;

  struct ncclSendMem* remDevMem = NULL;

  if (useMemcpy) {
    char shmPath[PATH_MAX];
    sprintf(shmPath, "/dev/shm/nccl-%s", info->shmName);
    TRACE(NCCL_SHM,"Open shmName %s shmSize %d", shmPath, info->shmSize);
    resources->shmSize = info->shmSize;
    NCCLCHECK(ncclShmOpen(shmPath, info->shmSize, (void**)&resources->shm, (void**)&resources->devShm, 0));
    // Remove the file to ensure proper clean-up
    NCCLCHECK(ncclShmUnlink(shmPath));

    recv->conn.tail = &resources->devShm->recvMem.tail;
    recv->conn.head = &resources->devShm->sendMem.head;
  } else {
    NCCLCHECK(p2pMap(comm->peerInfo+rank, comm->peerInfo+info->rank, &info->p2pBuff, (void**)&remDevMem, &resources->sendMemIpc));

    recv->conn.tail = &resources->devMem->tail;
    recv->conn.head = &remDevMem->head;
    recv->conn.ptrExchange = &remDevMem->ptrExchange;
    recv->conn.redOpArgExchange = remDevMem->redOpArgExchange;
  }

  char* buff = (char*)(resources->devMem+1);
  for (int p=0; p<NCCL_NUM_PROTOCOLS; p++) {
    if (info->read && p == NCCL_PROTO_SIMPLE) {
      if (remDevMem == NULL) return ncclInternalError; // We should not use read + memcpy
      /* For P2P Read the SIMPLE buffer is remote (ncclSendMem) */
      recv->conn.buffs[p] = (char*)(remDevMem+1);
    } else {
      recv->conn.buffs[p] = buff;
      buff += recv->comm->buffSizes[p];
    }
  }
#if (MERGE_MEMCPY == 1)
  int offsets[NCCL_STEPS];
  NCCLCHECK(ncclCudaCalloc(&recv->conn.offsFifo, NCCL_STEPS));
  int stepSize = recv->comm->buffSizes[NCCL_PROTO_SIMPLE]/NCCL_STEPS;
  for (int i=0; i<NCCL_STEPS; ++i) {
    offsets[i] = stepSize*(i*comm->nChannels + info->channelId + info->graphId*NCCL_STEPS*comm->nChannels);
  }
  NCCLCHECK(ncclCudaMemcpy(recv->conn.offsFifo, offsets, NCCL_STEPS));
#endif
  return ncclSuccess;
}

ncclResult_t p2pSendFree(struct ncclConnector* send) {
  struct p2pSendResources* resources = (struct p2pSendResources*)send->transportResources;
  if (resources->sendMemIpc) CUDACHECK(cudaIpcCloseMemHandle(resources->sendMemIpc));
  if (resources->recvMemIpc) CUDACHECK(cudaIpcCloseMemHandle(resources->recvMemIpc));
  free(resources);
  return ncclSuccess;
}

ncclResult_t p2pRecvFree(struct ncclConnector* recv) {
  struct p2pRecvResources* resources = (struct p2pRecvResources*)recv->transportResources;
  if (resources->sendMemIpc) CUDACHECK(cudaIpcCloseMemHandle(resources->sendMemIpc));
  if (resources->recvMemIpc) CUDACHECK(cudaIpcCloseMemHandle(resources->recvMemIpc));
  if (useMemcpy) {
    NCCLCHECK(ncclShmClose(resources->shm, resources->devShm, resources->shmSize));
  }
  free(resources);
  return ncclSuccess;
}

static ncclResult_t p2pSendProxySetup(struct ncclProxyConnection* connection, struct ncclComm* comm, void* reqBuff, int reqSize, void* respBuff, int respSize, int* done) {
  if (useMemcpy) {
    struct p2pProxyInfo* proxyInfo;
    NCCLCHECK(ncclCalloc(&proxyInfo, 1));
    connection->transportResources = proxyInfo;

#if (MERGE_MEMCPY == 1)
    if (comm->p2pProxySendMem) {
      comm->p2pProxySendBuffCnt++;
    } else {
      NCCLCHECK(ncclCudaCalloc(&comm->p2pProxySendMem, comm->buffSizes[NCCL_PROTO_SIMPLE]*comm->nChannels*3)); // 3: ring, tree, coll
      comm->p2pProxySendBuffCnt = 1;
    }
    proxyInfo->ceDevBuff = comm->p2pProxySendMem;
    NCCLCHECK(ncclCalloc(&proxyInfo->offsets, NCCL_STEPS));
#else
    NCCLCHECK(ncclCudaCalloc(&proxyInfo->ceDevBuff, comm->buffSizes[NCCL_PROTO_SIMPLE]));
#endif

    char shmPath[PATH_MAX];
    shmPath[0] = '\0';
    proxyInfo->shmSize = sizeof(struct ncclSendMem) + sizeof(struct ncclRecvMem);
    NCCLCHECK(ncclShmOpen(shmPath, proxyInfo->shmSize, (void**)&proxyInfo->shm, (void**)&proxyInfo->devShm, 1));
    TRACE(NCCL_SHM,"Opened shmName %s shmSize %d", shmPath, proxyInfo->shmSize);
    memcpy(proxyInfo->shmName, shmPath+sizeof("/dev/shm/nccl-")-1, sizeof(proxyInfo->shmName));

    NCCLCHECK(ncclCudaHostCalloc(&proxyInfo->ceRecvMem, 1));

    if (respSize != sizeof(struct p2pProxyInfo)) return ncclInternalError;
    memcpy(respBuff, proxyInfo, sizeof(struct p2pProxyInfo));
  } else {
    if (reqSize != sizeof(int)) return ncclInternalError;
    int size = *((int*)reqBuff);
    if (respSize != sizeof(struct ncclP2pBuff)) return ncclInternalError;
    struct ncclP2pBuff* p2pBuff = (struct ncclP2pBuff*)respBuff;
    NCCLCHECK(ncclCudaCalloc((char**)&p2pBuff->directPtr, size));
    connection->transportResources = p2pBuff->directPtr;
    cudaError_t res = cudaIpcGetMemHandle(&p2pBuff->devIpc, p2pBuff->directPtr);
    if (res != cudaSuccess) {
      WARN("cudaIpcGetMemHandle failed : %s", cudaGetErrorString(res));
      cudaFree(p2pBuff->directPtr);
      free(p2pBuff);
      CUDACHECK(res);
    }
  }
  *done = 1;
  return ncclSuccess;
}

static ncclResult_t p2pRecvProxySetup(struct ncclProxyConnection* connection, struct ncclComm* comm, void* reqBuff, int reqSize, void* respBuff, int respSize, int* done) {
  if (reqSize != sizeof(int)) return ncclInternalError;
  int size = *((int*)reqBuff);
  if (respSize != sizeof(struct ncclP2pBuff)) return ncclInternalError;
  struct ncclP2pBuff* p2pBuff = (struct ncclP2pBuff*)respBuff;
#if (MERGE_MEMCPY == 1)
  if (comm->p2pProxyRecvMem) {
    if (comm->p2pProxyRecvBuffSize != size) return ncclInternalError;
  } else {
    NCCLCHECK(ncclCudaCalloc(&comm->p2pProxyRecvMem, size*comm->nChannels*3)); // 3: ring, tree, coll
    comm->p2pProxyRecvBuffCnt = 1;
    comm->p2pProxyRecvBuffSize = size;
  }
  p2pBuff->directPtr = comm->p2pProxyRecvMem;
#else
  NCCLCHECK(ncclCudaCalloc((char**)&p2pBuff->directPtr, size));
#endif
  connection->transportResources = p2pBuff->directPtr;
  cudaError_t res = cudaIpcGetMemHandle(&p2pBuff->devIpc, p2pBuff->directPtr);
  if (res != cudaSuccess) {
    WARN("cudaIpcGetMemHandle failed : %s", cudaGetErrorString(res));
    cudaFree(p2pBuff->directPtr);
    free(p2pBuff);
    CUDACHECK(res);
  }
  *done = 1;
  return ncclSuccess;
}

static ncclResult_t p2pSendProxyConnect(struct ncclProxyConnection* connection, struct ncclComm* comm, void* reqBuff, int reqSize, void* respBuff, int respSize, int* done) {
  struct p2pProxyInfo* proxyInfo = (struct p2pProxyInfo*)connection->transportResources;

  if (reqSize != sizeof(void*)) return ncclInternalError;
  proxyInfo->recvFifo = *((char**)reqBuff);

  CUDACHECK(cudaStreamCreateWithFlags(&proxyInfo->stream, cudaStreamNonBlocking));
  for (int i=0; i<NCCL_STEPS; i++) {
    NCCLCHECK(p2pMemcpyEventCreate(proxyInfo->events+i));
  }
  connection->proxyAppendPtr = &connection->proxyAppend;
  return ncclSuccess;
}

static ncclResult_t p2pSendProxyFree(struct ncclProxyConnection* connection, struct ncclComm* comm) {
  if (useMemcpy) {
    struct p2pProxyInfo* proxyInfo = (struct p2pProxyInfo*)connection->transportResources;
    NCCLCHECK(ncclShmClose(proxyInfo->shm, proxyInfo->devShm, proxyInfo->shmSize));
    NCCLCHECK(ncclCudaHostFree(proxyInfo->ceRecvMem));
#if (MERGE_MEMCPY == 1)
    if (comm->p2pProxySendBuffCnt > 1) {
      if (--comm->p2pProxySendBuffCnt == 0) {
        CUDACHECK(cudaFree(comm->p2pProxySendMem));
        comm->p2pProxySendMem = nullptr;
      }
    }
#else
    CUDACHECK(cudaFree(proxyInfo->ceDevBuff));
#endif
    CUDACHECK(cudaStreamDestroy(proxyInfo->stream));
    for (int i=0; i<NCCL_STEPS; i++) {
      NCCLCHECK(p2pMemcpyEventDestroy(&proxyInfo->events[i]));
    }
    free(proxyInfo);
  } else {
    // Do not check return code as CUDA may have already shut down
    cudaFree(connection->transportResources);
  }
  return ncclSuccess;
}

static ncclResult_t p2pRecvProxyFree(struct ncclProxyConnection* connection, struct ncclComm* comm) {
  // Do not check return code as CUDA may have already shut down
  cudaFree(connection->transportResources);
  return ncclSuccess;
}

static ncclResult_t p2pSendProxyProgress(struct ncclComm* comm, struct ncclProxyArgs* args) {
  if (args->state == ncclProxyOpReady) {
    for (int s=0; s<args->nsubs; s++) {
      struct ncclProxySubArgs* sub = args->subs+s;
      struct p2pProxyInfo* resources = (struct p2pProxyInfo*) (sub->connection->transportResources);
      // Round to next multiple of sliceSteps
      sub->base = ROUNDUP(resources->step, args->chunkSteps);
      sub->posted = sub->transmitted = sub->done = 0;
    }
    args->state = ncclProxyOpProgress;
  }
  args->idle = 1;
  if (args->state == ncclProxyOpProgress) {
    int p = args->protocol;
#if (MERGE_MEMCPY == 0)
    int stepSize = comm->buffSizes[p] / NCCL_STEPS;
#endif
    for (int s=0; s<args->nsubs; s++) {
      struct ncclProxySubArgs* sub = args->subs+s;
      struct p2pProxyInfo* resources = (struct p2pProxyInfo*) (sub->connection->transportResources);
      if (p != NCCL_PROTO_SIMPLE) { // Only Simple uses cudaMemcpy
          resources->step = sub->base + sub->nsteps;
          args->done++;
          continue;
      }
      int cont;
#if (MERGE_MEMCPY == 1)
      cont = 1;
      while (sub->done < sub->transmitted && cont) {
        cont = 0;
        int buffSlot = (sub->base+sub->done)%NCCL_STEPS;
        int result;
        NCCLCHECK(p2pMemcpyEventQuery(&resources->events[buffSlot], &result));
        if (result) {
          // Untrack if it is a tracker
          NCCLCHECK(p2pMemcpyEventUntrack(&resources->events[buffSlot]));
          sub->done += args->sliceSteps;
          // Notify SHM
          resources->shm->recvMem.tail = sub->base + sub->done;
          cont = 1;
        }
        if (sub->done == sub->nsteps) {
          resources->step = sub->base + sub->nsteps;
          args->done++;
        }
      }
#endif
      cont = 1;
      while (sub->transmitted < sub->done + NCCL_STEPS && sub->transmitted < sub->nsteps && cont) {
        cont = 0;
        int buffSlot = (sub->base+sub->transmitted)%NCCL_STEPS;
#if (MERGE_MEMCPY == 0)
        volatile int* sizesFifo = resources->ceRecvMem->sizesFifo;
#endif
        volatile uint64_t* recvTail = &resources->ceRecvMem->tail;
        // Check GPU has sent everything
        if ((*recvTail > sub->base+sub->transmitted)) {
#if (MERGE_MEMCPY == 1)
          int bidx = -1;
          for (int i=0; i<2; ++i) {
            if (comm->memcpyDstBase[i] == nullptr) {
              comm->memcpyDstBase[i] = resources->recvFifo;
              bidx = i;
              break;
            } else if (comm->memcpyDstBase[i] == resources->recvFifo) {
              bidx = i;
              break;
            }
          }
          if (bidx == -1) return ncclInternalError;
          struct ncclMemcpyInfo* mi = &comm->memcpyInfo[bidx][comm->memcpyInfoCnt[bidx]];
          mi->proxyInfo = resources;
          mi->buffSlot = buffSlot;
          mi->channelId = sub->channelId;
          comm->memcpyInfoCnt[bidx]++;
#else
          int size = sizesFifo[buffSlot];
          CUDACHECK(cudaMemcpyAsync(resources->recvFifo+buffSlot*stepSize, resources->ceDevBuff+buffSlot*stepSize, size, cudaMemcpyDeviceToDevice, resources->stream));
          CUDACHECK(cudaEventRecord(resources->events[buffSlot], resources->stream));
#endif
          sub->transmitted += args->sliceSteps;
          cont = 1;
        }
      }
#if (MERGE_MEMCPY == 0)
      if (sub->done < sub->transmitted) {
        int buffSlot = (sub->base+sub->done)%NCCL_STEPS;
        cudaError_t res = cudaEventQuery(resources->events[buffSlot]);
        if (res != cudaErrorNotReady) CUDACHECK(res);
        if (res == cudaSuccess) {
          sub->done += args->sliceSteps;
          // Notify SHM
          resources->shm->recvMem.tail = sub->base + sub->done;
        }
        if (sub->done == sub->nsteps) {
          resources->step = sub->base + sub->nsteps;
          args->done++;
        }
      }
#endif
    }
    if (args->done == args->nsubs) {
      args->state = ncclProxyOpNone;
    }
  }
#if (MERGE_MEMCPY == 1)
  if (args->next == nullptr) {
    for (int bidx=0; bidx<2; ++bidx) {
      if (comm->memcpyInfoCnt[bidx] == 0) continue;
      struct p2pProxyInfo* rsrcs[NCCL_STEPS][MAXCHANNELS];
      int sizes[NCCL_STEPS][MAXCHANNELS];
      for (int i=0; i<NCCL_STEPS; ++i) {
        for (int j=0; j<MAXCHANNELS; ++j) {
          sizes[i][j] = 0;
        }
      }
      for (int i=0; i<comm->memcpyInfoCnt[bidx]; ++i) {
        struct p2pProxyInfo* resources = (struct p2pProxyInfo*)comm->memcpyInfo[bidx][i].proxyInfo;
        int buffSlot = comm->memcpyInfo[bidx][i].buffSlot;
        int channelId = comm->memcpyInfo[bidx][i].channelId;
        volatile int* sizesFifo = resources->ceRecvMem->sizesFifo;
        sizes[buffSlot][channelId] = sizesFifo[buffSlot];
        rsrcs[buffSlot][channelId] = resources;
      }
      int stepSize = comm->buffSizes[NCCL_PROTO_SIMPLE] / NCCL_STEPS;
      int cumStart = -1;
      int cumSize = 0;
      for (int i=0; i<NCCL_STEPS; ++i) {
        for (int j=0; j<comm->nChannels; ++j) {
          int size = sizes[i][j];
          if (size < stepSize || (size == stepSize && i == NCCL_STEPS-1 && j == comm->nChannels-1)) {
            if (cumStart == -1) {
              if (size > 0) {
                struct p2pProxyInfo* resources = rsrcs[i][j];
                char* dst = resources->recvFifo + resources->offsets[i];
                const char* src = resources->ceDevBuff + resources->offsets[i];
                CUDACHECK(cudaMemcpyAsync(dst, src, size, cudaMemcpyDeviceToDevice, resources->stream));
                NCCLCHECK(p2pMemcpyEventRecord(&resources->events[i], resources->stream));
              }
            } else {
              int iStart = cumStart / comm->nChannels;
              int jStart = cumStart % comm->nChannels;
              struct p2pProxyInfo* resources = rsrcs[iStart][jStart];
#if 0
              // No merging for debugging purposes
              for (int k=cumStart; k<j+comm->nChannels*i; ++k) {
                int ii = k / comm->nChannels;
                int jj = k % comm->nChannels;
                char* dst = rsrcs[ii][jj]->recvFifo + rsrcs[ii][jj]->offsets[ii];
                const char* src = rsrcs[ii][jj]->ceDevBuff + rsrcs[ii][jj]->offsets[ii];
                INFO(NCCL_ALL, "changho: ------ real copy dst %p src %p size %d", dst, src, sizes[ii][jj]);
                CUDACHECK(cudaMemcpyAsync(dst, src, sizes[ii][jj], cudaMemcpyDeviceToDevice, rsrcs[ii][jj]->stream));
                CUDACHECK(cudaEventRecord(rsrcs[ii][jj]->events[ii], rsrcs[ii][jj]->stream));
              }
              if (size > 0) {
                char* dst = rsrcs[i][j]->recvFifo + rsrcs[i][j]->offsets[i];
                const char* src = rsrcs[i][j]->ceDevBuff + rsrcs[i][j]->offsets[i];
                INFO(NCCL_ALL, "changho: ------ real copy dst %p src %p size %d", dst, src, sizes[i][j]);
                CUDACHECK(cudaMemcpyAsync(dst, src, sizes[i][j], cudaMemcpyDeviceToDevice, rsrcs[i][j]->stream));
                CUDACHECK(cudaEventRecord(rsrcs[i][j]->events[i], rsrcs[i][j]->stream));
              }
#else
              char* dst = resources->recvFifo + resources->offsets[iStart];
              const char* src = resources->ceDevBuff + resources->offsets[iStart];
              CUDACHECK(cudaMemcpyAsync(dst, src, cumSize+size, cudaMemcpyDeviceToDevice, resources->stream));
              NCCLCHECK(p2pMemcpyEventRecord(&resources->events[iStart], resources->stream));
              for (int k=cumStart+1; k<j+comm->nChannels*i; ++k) {
                int ii = k / comm->nChannels;
                int jj = k % comm->nChannels;
                NCCLCHECK(p2pMemcpyEventTrack(&rsrcs[ii][jj]->events[ii], &resources->events[iStart]));
              }
              if (size > 0) {
                NCCLCHECK(p2pMemcpyEventTrack(&rsrcs[i][j]->events[i], &resources->events[iStart]));
              }
#endif
              cumStart = -1;
              cumSize = 0;
            }
          } else if (size == stepSize) {
            if (cumStart == -1) {
              cumStart = j + comm->nChannels * i;
            }
            cumSize += size;
          } else {
            return ncclInternalError;
          }
        }
      }
      comm->memcpyInfoCnt[bidx] = 0;
    }
  }
#endif
  return ncclSuccess;
}

struct ncclTransport p2pTransport = {
  "P2P",
  p2pCanConnect,
  { p2pSendSetup, p2pSendConnect, p2pSendFree, NULL, p2pSendProxySetup, NULL, p2pSendProxyFree, NULL },
  { p2pRecvSetup, p2pRecvConnect, p2pRecvFree, NULL, p2pRecvProxySetup, NULL, p2pRecvProxyFree, NULL }
};

static void initCeOperation() {
  static int init = 0;
  if (!init) {
    useMemcpy = ncclParamP2pUseCudaMemcpy();
    if (useMemcpy) {
      p2pTransport.send.proxyConnect = p2pSendProxyConnect;
      p2pTransport.send.proxyProgress = p2pSendProxyProgress;
    }
    init = 1;
  }
}
