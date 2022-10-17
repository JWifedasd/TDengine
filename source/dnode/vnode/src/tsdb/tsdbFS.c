/*
 * Copyright (c) 2019 TAOS Data, Inc. <jhtao@taosdata.com>
 *
 * This program is free software: you can use, redistribute, and/or modify
 * it under the terms of the GNU Affero General Public License, version 3
 * or later ("AGPL"), as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "tsdb.h"

// =================================================================================================
int32_t tsdbEncodeFSToBinary(uint8_t *p, STsdbFS *pFS) {
  int32_t  n = 0;
  int8_t   hasDel = pFS->pDelFile ? 1 : 0;
  uint32_t nSet = taosArrayGetSize(pFS->aDFileSet);

  // version
  n += tPutI8(p ? p + n : p, 0);

  // SDelFile
  n += tPutI8(p ? p + n : p, hasDel);
  if (hasDel) {
    n += tPutDelFile(p ? p + n : p, pFS->pDelFile);
  }

  // SArray<SDFileSet>
  n += tPutU32v(p ? p + n : p, nSet);
  for (uint32_t iSet = 0; iSet < nSet; iSet++) {
    n += tPutDFileSet(p ? p + n : p, (SDFileSet *)taosArrayGet(pFS->aDFileSet, iSet));
  }

  return n;
}

static int32_t tsdbDecodeFSFromBinary(uint8_t *pData, int64_t nData, STsdbFS **ppFS) {
  int32_t  code = 0;
  int32_t  lino = 0;
  int8_t   hasDel = 0;
  uint32_t nSet = 0;
  int32_t  n = 0;
  STsdbFS *pFS = NULL;

  // alloc
  pFS = (STsdbFS *)taosMemoryCalloc(1, sizeof(*pFS));
  if (pFS == NULL) {
    code = TSDB_CODE_OUT_OF_MEMORY;
    TSDB_CHECK_CODE(code, lino, _exit);
  }

  // version
  n += tGetI8(pData + n, NULL);

  // SDelFile
  n += tGetI8(pData + n, &hasDel);
  if (hasDel) {
    pFS->pDelFile = (SDelFile *)taosMemoryCalloc(1, sizeof(SDelFile));
    if (pFS->pDelFile == NULL) {
      code = TSDB_CODE_OUT_OF_MEMORY;
      TSDB_CHECK_CODE(code, lino, _exit);
    }

    pFS->pDelFile->nRef = 1;
    n += tGetDelFile(pData + n, pFS->pDelFile);
  } else {
    pFS->pDelFile = NULL;
  }

  // SArray<SDFileSet>
  n += tGetU32v(pData + n, &nSet);

  pFS->aDFileSet = taosArrayInit(nSet, sizeof(SDFileSet));
  if (pFS->aDFileSet == NULL) {
    code = TSDB_CODE_OUT_OF_MEMORY;
    TSDB_CHECK_CODE(code, lino, _exit);
  }

  for (uint32_t iSet = 0; iSet < nSet; iSet++) {
    SDFileSet fSet = {0};

    int32_t nt = tGetDFileSet(pData + n, &fSet);
    if (nt < 0) {
      code = TSDB_CODE_OUT_OF_MEMORY;
      TSDB_CHECK_CODE(code, lino, _exit);
    }

    n += nt;

    if (taosArrayPush(pFS->aDFileSet, &fSet) == NULL) {
      code = TSDB_CODE_OUT_OF_MEMORY;
      TSDB_CHECK_CODE(code, lino, _exit);
    }
  }

  ASSERT(n + sizeof(TSCKSUM) == nData);

_exit:
  if (code) {
    *ppFS = NULL;
    tsdbError("%s failed at line %d since %s", __func__, lino, tstrerror(code));
    tsdbFSDestroy(pFS);
  } else {
    *ppFS = pFS;
  }
  return code;
}

static int32_t tsdbGnrtCurrent(STsdb *pTsdb, STsdbFS *pFS, char *fname) {
  int32_t   code = 0;
  int64_t   n;
  int64_t   size;
  uint8_t  *pData = NULL;
  TdFilePtr pFD = NULL;

  // to binary
  size = tsdbEncodeFSToBinary(NULL, pFS) + sizeof(TSCKSUM);
  pData = taosMemoryMalloc(size);
  if (pData == NULL) {
    code = TSDB_CODE_OUT_OF_MEMORY;
    goto _err;
  }
  n = tsdbEncodeFSToBinary(pData, pFS);
  ASSERT(n + sizeof(TSCKSUM) == size);
  taosCalcChecksumAppend(0, pData, size);

  // create and write
  pFD = taosOpenFile(fname, TD_FILE_WRITE | TD_FILE_CREATE | TD_FILE_TRUNC);
  if (pFD == NULL) {
    code = TAOS_SYSTEM_ERROR(errno);
    goto _err;
  }

  n = taosWriteFile(pFD, pData, size);
  if (n < 0) {
    code = TAOS_SYSTEM_ERROR(errno);
    goto _err;
  }

  if (taosFsyncFile(pFD) < 0) {
    code = TAOS_SYSTEM_ERROR(errno);
    goto _err;
  }

  taosCloseFile(&pFD);

  if (pData) taosMemoryFree(pData);
  return code;

_err:
  tsdbError("vgId:%d, tsdb gnrt current failed since %s", TD_VID(pTsdb->pVnode), tstrerror(code));
  if (pData) taosMemoryFree(pData);
  return code;
}

void tsdbFSDestroy(STsdbFS *pFS) {
  if (pFS == NULL) return;

  if (pFS->pDelFile) {
    taosMemoryFree(pFS->pDelFile);
  }

  for (int32_t iSet = 0; iSet < taosArrayGetSize(pFS->aDFileSet); iSet++) {
    SDFileSet *pSet = (SDFileSet *)taosArrayGet(pFS->aDFileSet, iSet);
    taosMemoryFree(pSet->pHeadF);
    taosMemoryFree(pSet->pDataF);
    taosMemoryFree(pSet->pSmaF);
    for (int32_t iStt = 0; iStt < pSet->nSttF; iStt++) {
      taosMemoryFree(pSet->aSttF[iStt]);
    }
  }

  taosArrayDestroy(pFS->aDFileSet);
  taosMemoryFree(pFS);
}

static int32_t tsdbScanAndTryFixFS(STsdb *pTsdb) {
  int32_t code = 0;
  int64_t size;
  char    fname[TSDB_FILENAME_LEN];

  // SDelFile
  if (pTsdb->fs.pDelFile) {
    tsdbDelFileName(pTsdb, pTsdb->fs.pDelFile, fname);
    if (taosStatFile(fname, &size, NULL)) {
      code = TAOS_SYSTEM_ERROR(errno);
      goto _err;
    }

    if (size != tsdbLogicToFileSize(pTsdb->fs.pDelFile->size, pTsdb->pVnode->config.tsdbPageSize)) {
      code = TSDB_CODE_FILE_CORRUPTED;
      goto _err;
    }
  }

  // SArray<SDFileSet>
  for (int32_t iSet = 0; iSet < taosArrayGetSize(pTsdb->fs.aDFileSet); iSet++) {
    SDFileSet *pSet = (SDFileSet *)taosArrayGet(pTsdb->fs.aDFileSet, iSet);

    // head =========
    tsdbHeadFileName(pTsdb, pSet->diskId, pSet->fid, pSet->pHeadF, fname);
    if (taosStatFile(fname, &size, NULL)) {
      code = TAOS_SYSTEM_ERROR(errno);
      goto _err;
    }
    if (size != tsdbLogicToFileSize(pSet->pHeadF->size, pTsdb->pVnode->config.tsdbPageSize)) {
      code = TSDB_CODE_FILE_CORRUPTED;
      goto _err;
    }

    // data =========
    tsdbDataFileName(pTsdb, pSet->diskId, pSet->fid, pSet->pDataF, fname);
    if (taosStatFile(fname, &size, NULL)) {
      code = TAOS_SYSTEM_ERROR(errno);
      goto _err;
    }
    if (size < tsdbLogicToFileSize(pSet->pDataF->size, pTsdb->pVnode->config.tsdbPageSize)) {
      code = TSDB_CODE_FILE_CORRUPTED;
      goto _err;
    } else if (size > tsdbLogicToFileSize(pSet->pDataF->size, pTsdb->pVnode->config.tsdbPageSize)) {
      code = tsdbDFileRollback(pTsdb, pSet, TSDB_DATA_FILE);
      if (code) goto _err;
    }

    // sma =============
    tsdbSmaFileName(pTsdb, pSet->diskId, pSet->fid, pSet->pSmaF, fname);
    if (taosStatFile(fname, &size, NULL)) {
      code = TAOS_SYSTEM_ERROR(errno);
      goto _err;
    }
    if (size < tsdbLogicToFileSize(pSet->pSmaF->size, pTsdb->pVnode->config.tsdbPageSize)) {
      code = TSDB_CODE_FILE_CORRUPTED;
      goto _err;
    } else if (size > tsdbLogicToFileSize(pSet->pSmaF->size, pTsdb->pVnode->config.tsdbPageSize)) {
      code = tsdbDFileRollback(pTsdb, pSet, TSDB_SMA_FILE);
      if (code) goto _err;
    }

    // stt ===========
    for (int32_t iStt = 0; iStt < pSet->nSttF; iStt++) {
      tsdbSttFileName(pTsdb, pSet->diskId, pSet->fid, pSet->aSttF[iStt], fname);
      if (taosStatFile(fname, &size, NULL)) {
        code = TAOS_SYSTEM_ERROR(errno);
        goto _err;
      }
      if (size != tsdbLogicToFileSize(pSet->aSttF[iStt]->size, pTsdb->pVnode->config.tsdbPageSize)) {
        code = TSDB_CODE_FILE_CORRUPTED;
        goto _err;
      }
    }
  }

  {
    // remove those invalid files (todo)
  }

  return code;

_err:
  tsdbError("vgId:%d, tsdb scan and try fix fs failed since %s", TD_VID(pTsdb->pVnode), tstrerror(code));
  return code;
}

int32_t tDFileSetCmprFn(const void *p1, const void *p2) {
  if (((SDFileSet *)p1)->fid < ((SDFileSet *)p2)->fid) {
    return -1;
  } else if (((SDFileSet *)p1)->fid > ((SDFileSet *)p2)->fid) {
    return 1;
  }

  return 0;
}

static void tsdbCurrentFileName(STsdb *pTsdb, char *current, char *current_t) {
  SVnode *pVnode = pTsdb->pVnode;
  if (pVnode->pTfs) {
    if (current) {
      snprintf(current, TSDB_FILENAME_LEN - 1, "%s%s%s%sCURRENT", tfsGetPrimaryPath(pTsdb->pVnode->pTfs), TD_DIRSEP,
               pTsdb->path, TD_DIRSEP);
    }
    if (current_t) {
      snprintf(current_t, TSDB_FILENAME_LEN - 1, "%s%s%s%sCURRENT.t", tfsGetPrimaryPath(pTsdb->pVnode->pTfs), TD_DIRSEP,
               pTsdb->path, TD_DIRSEP);
    }
  } else {
    if (current) {
      snprintf(current, TSDB_FILENAME_LEN - 1, "%s%sCURRENT", pTsdb->path, TD_DIRSEP);
    }
    if (current_t) {
      snprintf(current_t, TSDB_FILENAME_LEN - 1, "%s%sCURRENT.t", pTsdb->path, TD_DIRSEP);
    }
  }
}

static int32_t tsdbLoadFSFromFile(STsdb *pTsdb, const char *fname, STsdbFS *pFS) {
  int32_t  code = 0;
  int32_t  lino = 0;
  uint8_t *pData = NULL;
  int64_t  size = 0;

  TdFilePtr pFD = taosOpenFile(fname, TD_FILE_READ);
  if (pFD == NULL) {
    code = TAOS_SYSTEM_ERROR(errno);
    TSDB_CHECK_CODE(code, lino, _exit);
  }

  if (taosFStatFile(pFD, &size, NULL) < 0) {
    code = TAOS_SYSTEM_ERROR(errno);
    taosCloseFile(&pFD);
    TSDB_CHECK_CODE(code, lino, _exit);
  }

  pData = taosMemoryMalloc(size);
  if (pData == NULL) {
    code = TSDB_CODE_OUT_OF_MEMORY;
    taosCloseFile(&pFD);
    TSDB_CHECK_CODE(code, lino, _exit);
  }

  int64_t n = taosReadFile(pFD, pData, size);
  if (n < 0) {
    code = TAOS_SYSTEM_ERROR(errno);
    taosCloseFile(&pFD);
    TSDB_CHECK_CODE(code, lino, _exit);
  }

  if (!taosCheckChecksumWhole(pData, size)) {
    code = TSDB_CODE_FILE_CORRUPTED;
    taosCloseFile(&pFD);
    TSDB_CHECK_CODE(code, lino, _exit);
  }

  taosCloseFile(&pFD);

  // recover fs
  code = tsdbDecodeFSFromBinary(pData, size, &pFS);
  TSDB_CHECK_CODE(code, lino, _exit);

_exit:
  if (pData) taosMemoryFree(pData);
  if (code) {
    tsdbFSDestroy(pFS);
    tsdbError("vgId:%d %s failed at line %d since %s, fname: %s", TD_VID(pTsdb->pVnode), __func__, lino,
              tstrerror(code), fname);
  }
  return code;
}

// EXPOSED APIS ====================================================================================
int32_t tsdbFSOpen(STsdb *pTsdb, int8_t rollback) {
  int32_t code = 0;
  SVnode *pVnode = pTsdb->pVnode;

  // open handle
  pTsdb->fs.pDelFile = NULL;
  pTsdb->fs.aDFileSet = taosArrayInit(0, sizeof(SDFileSet));
  if (pTsdb->fs.aDFileSet == NULL) {
    code = TSDB_CODE_OUT_OF_MEMORY;
    goto _err;
  }

  // load fs or keep empty
  char current[TSDB_FILENAME_LEN] = {0};
  char current_t[TSDB_FILENAME_LEN] = {0};

  tsdbCurrentFileName(pTsdb, current, current_t);

  if (!taosCheckExistFile(current)) {
    // empty one
    code = tsdbGnrtCurrent(pTsdb, &pTsdb->fs, current);
    if (code) goto _err;
  } else {
    if (taosCheckExistFile(current_t)) {
      if (rollback) {
        (void)taosRemoveFile(current_t);
      } else {
        if (taosRenameFile(current_t, current) < 0) {
          code = TAOS_SYSTEM_ERROR(errno);
          goto _err;
        }
      }
    }

    code = tsdbLoadFSFromFile(pTsdb, current, &pTsdb->fs);
    if (code) goto _err;
  }

  // scan and fix FS
  code = tsdbScanAndTryFixFS(pTsdb);
  if (code) goto _err;

  return code;

_err:
  tsdbError("vgId:%d, tsdb fs open failed since %s", TD_VID(pTsdb->pVnode), tstrerror(code));
  return code;
}

int32_t tsdbFSClose(STsdb *pTsdb) {
  int32_t code = 0;

  if (pTsdb->fs.pDelFile) {
    ASSERT(pTsdb->fs.pDelFile->nRef == 1);
    taosMemoryFree(pTsdb->fs.pDelFile);
  }

  for (int32_t iSet = 0; iSet < taosArrayGetSize(pTsdb->fs.aDFileSet); iSet++) {
    SDFileSet *pSet = (SDFileSet *)taosArrayGet(pTsdb->fs.aDFileSet, iSet);

    // head
    ASSERT(pSet->pHeadF->nRef == 1);
    taosMemoryFree(pSet->pHeadF);

    // data
    ASSERT(pSet->pDataF->nRef == 1);
    taosMemoryFree(pSet->pDataF);

    // sma
    ASSERT(pSet->pSmaF->nRef == 1);
    taosMemoryFree(pSet->pSmaF);

    // stt
    for (int32_t iStt = 0; iStt < pSet->nSttF; iStt++) {
      ASSERT(pSet->aSttF[iStt]->nRef == 1);
      taosMemoryFree(pSet->aSttF[iStt]);
    }
  }

  taosArrayDestroy(pTsdb->fs.aDFileSet);

  return code;
}

int32_t tsdbFSCopy(STsdb *pTsdb, STsdbFS *pFS) {
  int32_t code = 0;

  pFS->pDelFile = NULL;
  pFS->aDFileSet = taosArrayInit(taosArrayGetSize(pTsdb->fs.aDFileSet), sizeof(SDFileSet));
  if (pFS->aDFileSet == NULL) {
    code = TSDB_CODE_OUT_OF_MEMORY;
    goto _exit;
  }

  if (pTsdb->fs.pDelFile) {
    pFS->pDelFile = (SDelFile *)taosMemoryMalloc(sizeof(SDelFile));
    if (pFS->pDelFile == NULL) {
      code = TSDB_CODE_OUT_OF_MEMORY;
      goto _exit;
    }

    *pFS->pDelFile = *pTsdb->fs.pDelFile;
  }

  for (int32_t iSet = 0; iSet < taosArrayGetSize(pTsdb->fs.aDFileSet); iSet++) {
    SDFileSet *pSet = (SDFileSet *)taosArrayGet(pTsdb->fs.aDFileSet, iSet);
    SDFileSet  fSet = {.diskId = pSet->diskId, .fid = pSet->fid};

    // head
    fSet.pHeadF = (SHeadFile *)taosMemoryMalloc(sizeof(SHeadFile));
    if (fSet.pHeadF == NULL) {
      code = TSDB_CODE_OUT_OF_MEMORY;
      goto _exit;
    }
    *fSet.pHeadF = *pSet->pHeadF;

    // data
    fSet.pDataF = (SDataFile *)taosMemoryMalloc(sizeof(SDataFile));
    if (fSet.pDataF == NULL) {
      code = TSDB_CODE_OUT_OF_MEMORY;
      goto _exit;
    }
    *fSet.pDataF = *pSet->pDataF;

    // sma
    fSet.pSmaF = (SSmaFile *)taosMemoryMalloc(sizeof(SSmaFile));
    if (fSet.pSmaF == NULL) {
      code = TSDB_CODE_OUT_OF_MEMORY;
      goto _exit;
    }
    *fSet.pSmaF = *pSet->pSmaF;

    // stt
    for (fSet.nSttF = 0; fSet.nSttF < pSet->nSttF; fSet.nSttF++) {
      fSet.aSttF[fSet.nSttF] = (SSttFile *)taosMemoryMalloc(sizeof(SSttFile));
      if (fSet.aSttF[fSet.nSttF] == NULL) {
        code = TSDB_CODE_OUT_OF_MEMORY;
        goto _exit;
      }
      *fSet.aSttF[fSet.nSttF] = *pSet->aSttF[fSet.nSttF];
    }

    if (taosArrayPush(pFS->aDFileSet, &fSet) == NULL) {
      code = TSDB_CODE_OUT_OF_MEMORY;
      goto _exit;
    }
  }

_exit:
  return code;
}

int32_t tsdbFSCommit(STsdb *pTsdb) {
  int32_t  code = 0;
  int32_t  lino = 0;
  STsdbFS *pFS = NULL;

  char current[TSDB_FILENAME_LEN] = {0};
  char current_t[TSDB_FILENAME_LEN] = {0};

  tsdbCurrentFileName(pTsdb, current, current_t);

  if (taosCheckExistFile(current_t)) {
    taosRenameFile(current_t, current);
  }

  if (!taosCheckExistFile(current)) {
    code = TSDB_CODE_FILE_CORRUPTED;
    TSDB_CHECK_CODE(code, lino, _exit);
  }

  // load FS (todo)
  // code = tsdbLoadFSFromFile(current, &pFS);
  TSDB_CHECK_CODE(code, lino, _exit);

  // merge new fs (todo)
  code = tsdbFSCommit2(pTsdb, pFS);
  TSDB_CHECK_CODE(code, lino, _exit);

_exit:
  if (pFS) tsdbFSDestroy(pFS);
  if (code) {
    tsdbError("vgId:%d %s failed at line %d since %s", TD_VID(pTsdb->pVnode), __func__, lino, tstrerror(code));
  }
  return code;
}

int32_t tsdbFSRollback(STsdb *pTsdb) {
  int32_t code = 0;

  ASSERT(0);

  return code;
}

int32_t tsdbFSUpsertDelFile(STsdbFS *pFS, SDelFile *pDelFile) {
  int32_t code = 0;

  if (pFS->pDelFile == NULL) {
    pFS->pDelFile = (SDelFile *)taosMemoryMalloc(sizeof(SDelFile));
    if (pFS->pDelFile == NULL) {
      code = TSDB_CODE_OUT_OF_MEMORY;
      goto _exit;
    }
  }
  *pFS->pDelFile = *pDelFile;

_exit:
  return code;
}

int32_t tsdbFSUpsertFSet(STsdbFS *pFS, SDFileSet *pSet) {
  int32_t code = 0;
  int32_t idx = taosArraySearchIdx(pFS->aDFileSet, pSet, tDFileSetCmprFn, TD_GE);

  if (idx < 0) {
    idx = taosArrayGetSize(pFS->aDFileSet);
  } else {
    SDFileSet *pDFileSet = (SDFileSet *)taosArrayGet(pFS->aDFileSet, idx);
    int32_t    c = tDFileSetCmprFn(pSet, pDFileSet);
    if (c == 0) {
      *pDFileSet->pHeadF = *pSet->pHeadF;
      *pDFileSet->pDataF = *pSet->pDataF;
      *pDFileSet->pSmaF = *pSet->pSmaF;
      // stt
      if (pSet->nSttF > pDFileSet->nSttF) {
        ASSERT(pSet->nSttF == pDFileSet->nSttF + 1);

        pDFileSet->aSttF[pDFileSet->nSttF] = (SSttFile *)taosMemoryMalloc(sizeof(SSttFile));
        if (pDFileSet->aSttF[pDFileSet->nSttF] == NULL) {
          code = TSDB_CODE_OUT_OF_MEMORY;
          goto _exit;
        }
        *pDFileSet->aSttF[pDFileSet->nSttF] = *pSet->aSttF[pSet->nSttF - 1];
        pDFileSet->nSttF++;
      } else if (pSet->nSttF < pDFileSet->nSttF) {
        ASSERT(pSet->nSttF == 1);
        for (int32_t iStt = 1; iStt < pDFileSet->nSttF; iStt++) {
          taosMemoryFree(pDFileSet->aSttF[iStt]);
        }

        *pDFileSet->aSttF[0] = *pSet->aSttF[0];
        pDFileSet->nSttF = 1;
      } else {
        for (int32_t iStt = 0; iStt < pSet->nSttF; iStt++) {
          *pDFileSet->aSttF[iStt] = *pSet->aSttF[iStt];
        }
      }

      goto _exit;
    }
  }

  ASSERT(pSet->nSttF == 1);
  SDFileSet fSet = {.diskId = pSet->diskId, .fid = pSet->fid, .nSttF = 1};

  // head
  fSet.pHeadF = (SHeadFile *)taosMemoryMalloc(sizeof(SHeadFile));
  if (fSet.pHeadF == NULL) {
    code = TSDB_CODE_OUT_OF_MEMORY;
    goto _exit;
  }
  *fSet.pHeadF = *pSet->pHeadF;

  // data
  fSet.pDataF = (SDataFile *)taosMemoryMalloc(sizeof(SDataFile));
  if (fSet.pDataF == NULL) {
    code = TSDB_CODE_OUT_OF_MEMORY;
    goto _exit;
  }
  *fSet.pDataF = *pSet->pDataF;

  // sma
  fSet.pSmaF = (SSmaFile *)taosMemoryMalloc(sizeof(SSmaFile));
  if (fSet.pSmaF == NULL) {
    code = TSDB_CODE_OUT_OF_MEMORY;
    goto _exit;
  }
  *fSet.pSmaF = *pSet->pSmaF;

  // stt
  fSet.aSttF[0] = (SSttFile *)taosMemoryMalloc(sizeof(SSttFile));
  if (fSet.aSttF[0] == NULL) {
    code = TSDB_CODE_OUT_OF_MEMORY;
    goto _exit;
  }
  *fSet.aSttF[0] = *pSet->aSttF[0];

  if (taosArrayInsert(pFS->aDFileSet, idx, &fSet) == NULL) {
    code = TSDB_CODE_OUT_OF_MEMORY;
    goto _exit;
  }

_exit:
  return code;
}

int32_t tsdbFSCommit1(STsdb *pTsdb, STsdbFS *pFSNew) {
  int32_t code = 0;
  char    tfname[TSDB_FILENAME_LEN];
  char    fname[TSDB_FILENAME_LEN];

  snprintf(tfname, TSDB_FILENAME_LEN - 1, "%s%s%s%sCURRENT.t", tfsGetPrimaryPath(pTsdb->pVnode->pTfs), TD_DIRSEP,
           pTsdb->path, TD_DIRSEP);
  snprintf(fname, TSDB_FILENAME_LEN - 1, "%s%s%s%sCURRENT", tfsGetPrimaryPath(pTsdb->pVnode->pTfs), TD_DIRSEP,
           pTsdb->path, TD_DIRSEP);

  // gnrt CURRENT.t
  code = tsdbGnrtCurrent(pTsdb, pFSNew, tfname);
  if (code) goto _err;

  // rename
  code = taosRenameFile(tfname, fname);
  if (code) {
    code = TAOS_SYSTEM_ERROR(code);
    goto _err;
  }

  return code;

_err:
  tsdbError("vgId:%d, tsdb fs commit phase 1 failed since %s", TD_VID(pTsdb->pVnode), tstrerror(code));
  return code;
}

int32_t tsdbFSCommit2(STsdb *pTsdb, STsdbFS *pFSNew) {
  int32_t code = 0;
  int32_t nRef;
  char    fname[TSDB_FILENAME_LEN];

  // del
  if (pFSNew->pDelFile) {
    SDelFile *pDelFile = pTsdb->fs.pDelFile;

    if (pDelFile == NULL || (pDelFile->commitID != pFSNew->pDelFile->commitID)) {
      pTsdb->fs.pDelFile = (SDelFile *)taosMemoryMalloc(sizeof(SDelFile));
      if (pTsdb->fs.pDelFile == NULL) {
        code = TSDB_CODE_OUT_OF_MEMORY;
        goto _err;
      }

      *pTsdb->fs.pDelFile = *pFSNew->pDelFile;
      pTsdb->fs.pDelFile->nRef = 1;

      if (pDelFile) {
        nRef = atomic_sub_fetch_32(&pDelFile->nRef, 1);
        if (nRef == 0) {
          tsdbDelFileName(pTsdb, pDelFile, fname);
          taosRemoveFile(fname);
          taosMemoryFree(pDelFile);
        }
      }
    }
  } else {
    ASSERT(pTsdb->fs.pDelFile == NULL);
  }

  // data
  int32_t iOld = 0;
  int32_t iNew = 0;
  while (true) {
    int32_t   nOld = taosArrayGetSize(pTsdb->fs.aDFileSet);
    int32_t   nNew = taosArrayGetSize(pFSNew->aDFileSet);
    SDFileSet fSet;
    int8_t    sameDisk;

    if (iOld >= nOld && iNew >= nNew) break;

    SDFileSet *pSetOld = (iOld < nOld) ? taosArrayGet(pTsdb->fs.aDFileSet, iOld) : NULL;
    SDFileSet *pSetNew = (iNew < nNew) ? taosArrayGet(pFSNew->aDFileSet, iNew) : NULL;

    if (pSetOld && pSetNew) {
      if (pSetOld->fid == pSetNew->fid) {
        goto _merge_old_and_new;
      } else if (pSetOld->fid < pSetNew->fid) {
        goto _remove_old;
      } else {
        goto _add_new;
      }
    } else if (pSetOld) {
      goto _remove_old;
    } else {
      goto _add_new;
    }

  _merge_old_and_new:
    sameDisk = ((pSetOld->diskId.level == pSetNew->diskId.level) && (pSetOld->diskId.id == pSetNew->diskId.id));

    // head
    fSet.pHeadF = pSetOld->pHeadF;
    if ((!sameDisk) || (pSetOld->pHeadF->commitID != pSetNew->pHeadF->commitID)) {
      pSetOld->pHeadF = (SHeadFile *)taosMemoryMalloc(sizeof(SHeadFile));
      if (pSetOld->pHeadF == NULL) {
        code = TSDB_CODE_OUT_OF_MEMORY;
        goto _err;
      }
      *pSetOld->pHeadF = *pSetNew->pHeadF;
      pSetOld->pHeadF->nRef = 1;

      nRef = atomic_sub_fetch_32(&fSet.pHeadF->nRef, 1);
      if (nRef == 0) {
        tsdbHeadFileName(pTsdb, pSetOld->diskId, pSetOld->fid, fSet.pHeadF, fname);
        (void)taosRemoveFile(fname);
        taosMemoryFree(fSet.pHeadF);
      }
    } else {
      ASSERT(fSet.pHeadF->size == pSetNew->pHeadF->size);
      ASSERT(fSet.pHeadF->offset == pSetNew->pHeadF->offset);
    }

    // data
    fSet.pDataF = pSetOld->pDataF;
    if ((!sameDisk) || (pSetOld->pDataF->commitID != pSetNew->pDataF->commitID)) {
      pSetOld->pDataF = (SDataFile *)taosMemoryMalloc(sizeof(SDataFile));
      if (pSetOld->pDataF == NULL) {
        code = TSDB_CODE_OUT_OF_MEMORY;
        goto _err;
      }
      *pSetOld->pDataF = *pSetNew->pDataF;
      pSetOld->pDataF->nRef = 1;

      nRef = atomic_sub_fetch_32(&fSet.pDataF->nRef, 1);
      if (nRef == 0) {
        tsdbDataFileName(pTsdb, pSetOld->diskId, pSetOld->fid, fSet.pDataF, fname);
        taosRemoveFile(fname);
        taosMemoryFree(fSet.pDataF);
      }
    } else {
      ASSERT(pSetOld->pDataF->size <= pSetNew->pDataF->size);
      pSetOld->pDataF->size = pSetNew->pDataF->size;
    }

    // sma
    fSet.pSmaF = pSetOld->pSmaF;
    if ((!sameDisk) || (pSetOld->pSmaF->commitID != pSetNew->pSmaF->commitID)) {
      pSetOld->pSmaF = (SSmaFile *)taosMemoryMalloc(sizeof(SSmaFile));
      if (pSetOld->pSmaF == NULL) {
        code = TSDB_CODE_OUT_OF_MEMORY;
        goto _err;
      }
      *pSetOld->pSmaF = *pSetNew->pSmaF;
      pSetOld->pSmaF->nRef = 1;

      nRef = atomic_sub_fetch_32(&fSet.pSmaF->nRef, 1);
      if (nRef == 0) {
        tsdbSmaFileName(pTsdb, pSetOld->diskId, pSetOld->fid, fSet.pSmaF, fname);
        (void)taosRemoveFile(fname);
        taosMemoryFree(fSet.pSmaF);
      }
    } else {
      ASSERT(pSetOld->pSmaF->size <= pSetNew->pSmaF->size);
      pSetOld->pSmaF->size = pSetNew->pSmaF->size;
    }

    // stt
    if (sameDisk) {
      if (pSetNew->nSttF > pSetOld->nSttF) {
        ASSERT(pSetNew->nSttF == pSetOld->nSttF + 1);
        pSetOld->aSttF[pSetOld->nSttF] = (SSttFile *)taosMemoryMalloc(sizeof(SSttFile));
        if (pSetOld->aSttF[pSetOld->nSttF] == NULL) {
          code = TSDB_CODE_OUT_OF_MEMORY;
          goto _err;
        }
        *pSetOld->aSttF[pSetOld->nSttF] = *pSetNew->aSttF[pSetOld->nSttF];
        pSetOld->aSttF[pSetOld->nSttF]->nRef = 1;
        pSetOld->nSttF++;
      } else if (pSetNew->nSttF < pSetOld->nSttF) {
        ASSERT(pSetNew->nSttF == 1);
        for (int32_t iStt = 0; iStt < pSetOld->nSttF; iStt++) {
          SSttFile *pSttFile = pSetOld->aSttF[iStt];
          nRef = atomic_sub_fetch_32(&pSttFile->nRef, 1);
          if (nRef == 0) {
            tsdbSttFileName(pTsdb, pSetOld->diskId, pSetOld->fid, pSttFile, fname);
            taosRemoveFile(fname);
            taosMemoryFree(pSttFile);
          }
          pSetOld->aSttF[iStt] = NULL;
        }

        pSetOld->nSttF = 1;
        pSetOld->aSttF[0] = (SSttFile *)taosMemoryMalloc(sizeof(SSttFile));
        if (pSetOld->aSttF[0] == NULL) {
          code = TSDB_CODE_OUT_OF_MEMORY;
          goto _err;
        }
        *pSetOld->aSttF[0] = *pSetNew->aSttF[0];
        pSetOld->aSttF[0]->nRef = 1;
      } else {
        for (int32_t iStt = 0; iStt < pSetOld->nSttF; iStt++) {
          if (pSetOld->aSttF[iStt]->commitID != pSetNew->aSttF[iStt]->commitID) {
            SSttFile *pSttFile = pSetOld->aSttF[iStt];
            nRef = atomic_sub_fetch_32(&pSttFile->nRef, 1);
            if (nRef == 0) {
              tsdbSttFileName(pTsdb, pSetOld->diskId, pSetOld->fid, pSttFile, fname);
              taosRemoveFile(fname);
              taosMemoryFree(pSttFile);
            }

            pSetOld->aSttF[iStt] = (SSttFile *)taosMemoryMalloc(sizeof(SSttFile));
            if (pSetOld->aSttF[iStt] == NULL) {
              code = TSDB_CODE_OUT_OF_MEMORY;
              goto _err;
            }
            *pSetOld->aSttF[iStt] = *pSetNew->aSttF[iStt];
            pSetOld->aSttF[iStt]->nRef = 1;
          } else {
            ASSERT(pSetOld->aSttF[iStt]->size == pSetOld->aSttF[iStt]->size);
            ASSERT(pSetOld->aSttF[iStt]->offset == pSetOld->aSttF[iStt]->offset);
          }
        }
      }
    } else {
      ASSERT(pSetOld->nSttF == pSetNew->nSttF);
      for (int32_t iStt = 0; iStt < pSetOld->nSttF; iStt++) {
        SSttFile *pSttFile = pSetOld->aSttF[iStt];
        nRef = atomic_sub_fetch_32(&pSttFile->nRef, 1);
        if (nRef == 0) {
          tsdbSttFileName(pTsdb, pSetOld->diskId, pSetOld->fid, pSttFile, fname);
          taosRemoveFile(fname);
          taosMemoryFree(pSttFile);
        }

        pSetOld->aSttF[iStt] = (SSttFile *)taosMemoryMalloc(sizeof(SSttFile));
        if (pSetOld->aSttF[iStt] == NULL) {
          code = TSDB_CODE_OUT_OF_MEMORY;
          goto _err;
        }
        *pSetOld->aSttF[iStt] = *pSetNew->aSttF[iStt];
        pSetOld->aSttF[iStt]->nRef = 1;
      }
    }

    if (!sameDisk) {
      pSetOld->diskId = pSetNew->diskId;
    }

    iOld++;
    iNew++;
    continue;

  _remove_old:
    nRef = atomic_sub_fetch_32(&pSetOld->pHeadF->nRef, 1);
    if (nRef == 0) {
      tsdbHeadFileName(pTsdb, pSetOld->diskId, pSetOld->fid, pSetOld->pHeadF, fname);
      (void)taosRemoveFile(fname);
      taosMemoryFree(pSetOld->pHeadF);
    }

    nRef = atomic_sub_fetch_32(&pSetOld->pDataF->nRef, 1);
    if (nRef == 0) {
      tsdbDataFileName(pTsdb, pSetOld->diskId, pSetOld->fid, pSetOld->pDataF, fname);
      taosRemoveFile(fname);
      taosMemoryFree(pSetOld->pDataF);
    }

    nRef = atomic_sub_fetch_32(&pSetOld->pSmaF->nRef, 1);
    if (nRef == 0) {
      tsdbSmaFileName(pTsdb, pSetOld->diskId, pSetOld->fid, pSetOld->pSmaF, fname);
      taosRemoveFile(fname);
      taosMemoryFree(pSetOld->pSmaF);
    }

    for (int8_t iStt = 0; iStt < pSetOld->nSttF; iStt++) {
      nRef = atomic_sub_fetch_32(&pSetOld->aSttF[iStt]->nRef, 1);
      if (nRef == 0) {
        tsdbSttFileName(pTsdb, pSetOld->diskId, pSetOld->fid, pSetOld->aSttF[iStt], fname);
        taosRemoveFile(fname);
        taosMemoryFree(pSetOld->aSttF[iStt]);
      }
    }

    taosArrayRemove(pTsdb->fs.aDFileSet, iOld);
    continue;

  _add_new:
    fSet = (SDFileSet){.diskId = pSetNew->diskId, .fid = pSetNew->fid, .nSttF = 1};

    // head
    fSet.pHeadF = (SHeadFile *)taosMemoryMalloc(sizeof(SHeadFile));
    if (fSet.pHeadF == NULL) {
      code = TSDB_CODE_OUT_OF_MEMORY;
      goto _err;
    }
    *fSet.pHeadF = *pSetNew->pHeadF;
    fSet.pHeadF->nRef = 1;

    // data
    fSet.pDataF = (SDataFile *)taosMemoryMalloc(sizeof(SDataFile));
    if (fSet.pDataF == NULL) {
      code = TSDB_CODE_OUT_OF_MEMORY;
      goto _err;
    }
    *fSet.pDataF = *pSetNew->pDataF;
    fSet.pDataF->nRef = 1;

    // sma
    fSet.pSmaF = (SSmaFile *)taosMemoryMalloc(sizeof(SSmaFile));
    if (fSet.pSmaF == NULL) {
      code = TSDB_CODE_OUT_OF_MEMORY;
      goto _err;
    }
    *fSet.pSmaF = *pSetNew->pSmaF;
    fSet.pSmaF->nRef = 1;

    // stt
    ASSERT(pSetNew->nSttF == 1);
    fSet.aSttF[0] = (SSttFile *)taosMemoryMalloc(sizeof(SSttFile));
    if (fSet.aSttF[0] == NULL) {
      code = TSDB_CODE_OUT_OF_MEMORY;
      goto _err;
    }
    *fSet.aSttF[0] = *pSetNew->aSttF[0];
    fSet.aSttF[0]->nRef = 1;

    if (taosArrayInsert(pTsdb->fs.aDFileSet, iOld, &fSet) == NULL) {
      code = TSDB_CODE_OUT_OF_MEMORY;
      goto _err;
    }
    iOld++;
    iNew++;
    continue;
  }

  return code;

_err:
  tsdbError("vgId:%d, tsdb fs commit phase 2 failed since %s", TD_VID(pTsdb->pVnode), tstrerror(code));
  return code;
}

int32_t tsdbFSRef(STsdb *pTsdb, STsdbFS *pFS) {
  int32_t code = 0;
  int32_t nRef;

  pFS->aDFileSet = taosArrayInit(taosArrayGetSize(pTsdb->fs.aDFileSet), sizeof(SDFileSet));
  if (pFS->aDFileSet == NULL) {
    code = TSDB_CODE_OUT_OF_MEMORY;
    goto _exit;
  }

  pFS->pDelFile = pTsdb->fs.pDelFile;
  if (pFS->pDelFile) {
    nRef = atomic_fetch_add_32(&pFS->pDelFile->nRef, 1);
    ASSERT(nRef > 0);
  }

  SDFileSet fSet;
  for (int32_t iSet = 0; iSet < taosArrayGetSize(pTsdb->fs.aDFileSet); iSet++) {
    SDFileSet *pSet = (SDFileSet *)taosArrayGet(pTsdb->fs.aDFileSet, iSet);
    fSet = *pSet;

    nRef = atomic_fetch_add_32(&pSet->pHeadF->nRef, 1);
    ASSERT(nRef > 0);

    nRef = atomic_fetch_add_32(&pSet->pDataF->nRef, 1);
    ASSERT(nRef > 0);

    nRef = atomic_fetch_add_32(&pSet->pSmaF->nRef, 1);
    ASSERT(nRef > 0);

    for (int32_t iStt = 0; iStt < pSet->nSttF; iStt++) {
      nRef = atomic_fetch_add_32(&pSet->aSttF[iStt]->nRef, 1);
      ASSERT(nRef > 0);
    }

    if (taosArrayPush(pFS->aDFileSet, &fSet) == NULL) {
      code = TSDB_CODE_OUT_OF_MEMORY;
      goto _exit;
    }
  }

_exit:
  return code;
}

void tsdbFSUnref(STsdb *pTsdb, STsdbFS *pFS) {
  int32_t nRef;
  char    fname[TSDB_FILENAME_LEN];

  if (pFS->pDelFile) {
    nRef = atomic_sub_fetch_32(&pFS->pDelFile->nRef, 1);
    ASSERT(nRef >= 0);
    if (nRef == 0) {
      tsdbDelFileName(pTsdb, pFS->pDelFile, fname);
      (void)taosRemoveFile(fname);
      taosMemoryFree(pFS->pDelFile);
    }
  }

  for (int32_t iSet = 0; iSet < taosArrayGetSize(pFS->aDFileSet); iSet++) {
    SDFileSet *pSet = (SDFileSet *)taosArrayGet(pFS->aDFileSet, iSet);

    // head
    nRef = atomic_sub_fetch_32(&pSet->pHeadF->nRef, 1);
    ASSERT(nRef >= 0);
    if (nRef == 0) {
      tsdbHeadFileName(pTsdb, pSet->diskId, pSet->fid, pSet->pHeadF, fname);
      (void)taosRemoveFile(fname);
      taosMemoryFree(pSet->pHeadF);
    }

    // data
    nRef = atomic_sub_fetch_32(&pSet->pDataF->nRef, 1);
    ASSERT(nRef >= 0);
    if (nRef == 0) {
      tsdbDataFileName(pTsdb, pSet->diskId, pSet->fid, pSet->pDataF, fname);
      taosRemoveFile(fname);
      taosMemoryFree(pSet->pDataF);
    }

    // sma
    nRef = atomic_sub_fetch_32(&pSet->pSmaF->nRef, 1);
    ASSERT(nRef >= 0);
    if (nRef == 0) {
      tsdbSmaFileName(pTsdb, pSet->diskId, pSet->fid, pSet->pSmaF, fname);
      taosRemoveFile(fname);
      taosMemoryFree(pSet->pSmaF);
    }

    // stt
    for (int32_t iStt = 0; iStt < pSet->nSttF; iStt++) {
      nRef = atomic_sub_fetch_32(&pSet->aSttF[iStt]->nRef, 1);
      ASSERT(nRef >= 0);
      if (nRef == 0) {
        tsdbSttFileName(pTsdb, pSet->diskId, pSet->fid, pSet->aSttF[iStt], fname);
        taosRemoveFile(fname);
        taosMemoryFree(pSet->aSttF[iStt]);
        /* code */
      }
    }
  }

  taosArrayDestroy(pFS->aDFileSet);
}