/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
 * MemFabric_Hybrid is licensed under Mulan PSL v2.
 * You can use this software according to the terms and conditions of the Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *		  http://license.coscl.org.cn/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PSL v2 for more details.
 */
package main

/*
#cgo CFLAGS: -I${SRCDIR}/../../../smem/include/host

#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdbool.h>
#include "smem_def.h"

// Opaque pointer definition
typedef struct EtcdClient EtcdClient;

static inline bool call_fill_callback(smem_store_prefix_get_ctx_t* ctx,
									  const char* key,
									  const void* value,
									  uint64_t size,
									  void* context) {
	return ctx->fill(key, value, size, context);
}
*/
import "C"

import (
	"context"
	"fmt"
	"os"
	"runtime/cgo"
	"strings"
	"sync"
	"time"
	"unsafe"

	clientv3 "go.etcd.io/etcd/client/v3"
	"go.etcd.io/etcd/client/v3/concurrency"
)

// -----------------------------------------------------------------------------
// Constants & Configuration
// -----------------------------------------------------------------------------

const (
	DefaultLockKey = "/memfabric_hybrid/etcd_client/global_lock"

	// DefaultOpTimeout defines the default timeout for internal etcd operations
	// if the client-level timeout is too loose.
	DefaultOpTimeout = 5 * time.Second

	// SessionTTL defines the TTL for distributed lock session.
	// If process dies, lock is released after this duration.
	SessionTTL = 10

	// LockAcquireTimeout bounds how long a distributed lock acquisition waits.
	// A stale lock key from a killed leader must not wedge the election loop.
	LockAcquireTimeout = 30 * time.Second

	// CleanupTimeout defines the timeout for cleanup operations during Close.
	CleanupTimeout = 2 * time.Second
)

// -----------------------------------------------------------------------------
// Struct Definitions
// -----------------------------------------------------------------------------

// etcdWrapper encapsulates the etcd client and its state.
// It guarantees thread-safety for C callers.
type etcdWrapper struct {
	client  *clientv3.Client
	timeout time.Duration

	// mu protects mutable fields (lastErrC, session, mutex, keepAliveCancel)
	mu sync.Mutex

	// lastErrC holds the C-allocated string of the last error.
	// Contract: Owned by Go, freed by Go before update or on close.
	// Read-only for C callers via Etcd_GetLastError.
	lastErrC *C.char

	// keepAliveCancels tracks all active lease keepalive goroutines.
	// Each lease gets its own independent goroutine; cancelling one
	// does not affect others. Keyed by lease ID.
	keepAliveCancels map[clientv3.LeaseID]context.CancelFunc

	// Concurrency primitives for distributed lock
	session *concurrency.Session
	mutex   *concurrency.Mutex
}

// -----------------------------------------------------------------------------
// Helper Functions
// -----------------------------------------------------------------------------

// setError updates the error state safely.
// Caller must NOT hold w.mu when calling this method.
func (w *etcdWrapper) setError(err error) {
	w.mu.Lock()
	defer w.mu.Unlock()
	w.setErrorLocked(err)
}

// setErrorLocked updates the error state.
// Caller MUST hold w.mu when calling this method.
func (w *etcdWrapper) setErrorLocked(err error) {
	if w.lastErrC != nil {
		C.free(unsafe.Pointer(w.lastErrC))
		w.lastErrC = nil
	}

	if err != nil {
		w.lastErrC = C.CString(err.Error())
	}
}

// getWrapper retrieves the Go struct from the C handle safely.
func getWrapper(ptr *C.EtcdClient) *etcdWrapper {
	if ptr == nil {
		return nil
	}
	defer func() {
		if r := recover(); r != nil {
			fmt.Fprintf(os.Stderr, "[etcd-cgo] getWrapper panic recovered: %v\n", r)
		}
	}()

	h := cgo.Handle(uintptr(unsafe.Pointer(ptr)))
	if v, ok := h.Value().(*etcdWrapper); ok {
		return v
	}
	return nil
}

// drainKeepAlive consumes the keepalive channel to prevent blocking.
// It runs until the context is canceled or the channel closes.
// When done, it removes itself from the wrapper's keepAliveCancels map.
func drainKeepAlive(ctx context.Context, ch <-chan *clientv3.LeaseKeepAliveResponse, leaseID clientv3.LeaseID, w *etcdWrapper) {
	if ch == nil {
		return
	}
	defer func() {
		w.mu.Lock()
		delete(w.keepAliveCancels, leaseID)
		w.mu.Unlock()
	}()
	for {
		select {
		case <-ctx.Done():
			return
		case resp, ok := <-ch:
			if !ok {
				return
			}
			if resp == nil {
				fmt.Fprintf(os.Stderr, "[etcd-cgo] lease keepalive lost\n")
			}
		}
	}
}

// -----------------------------------------------------------------------------
// Exported C API
// -----------------------------------------------------------------------------

//export Etcd_New
func Etcd_New(endpoints *C.char, username *C.char, password *C.char, timeoutSeconds C.int) *C.EtcdClient {
	if endpoints == nil {
		return nil
	}

	goEndpoints := C.GoString(endpoints)
	goUsername := ""
	if username != nil {
		goUsername = C.GoString(username)
	}
	goPassword := ""
	if password != nil {
		goPassword = C.GoString(password)
	}

	eps := strings.Split(goEndpoints, ",")
	dur := time.Duration(timeoutSeconds) * time.Second
	if dur <= 0 {
		dur = DefaultOpTimeout
	}

	config := clientv3.Config{
		Endpoints:   eps,
		DialTimeout: dur,
		Username:    goUsername,
		Password:    goPassword,
		// Only send keepalive PINGs while an RPC stream is active. Sending
		// pings on idle connections (PermitWithoutStream=true) with a short
		// interval triggers the etcd server's keepalive enforcement policy,
		// which responds with GOAWAY "too_many_pings" and silently drops the
		// connection (first Get then fails with context deadline exceeded).
		PermitWithoutStream:  false,
		DialKeepAliveTime:    30 * time.Second,
		DialKeepAliveTimeout: 3 * time.Second,
		AutoSyncInterval:     time.Minute,
	}

	cli, err := clientv3.New(config)
	if err != nil {
		fmt.Fprintf(os.Stderr, "[etcd-cgo] Etcd_New failed: %v\n", err)
		return nil
	}

	wrapper := &etcdWrapper{
		client:  cli,
		timeout: dur,
	}

	h := cgo.NewHandle(wrapper)
	return (*C.EtcdClient)(unsafe.Pointer(uintptr(h)))
}

//export Etcd_Close
func Etcd_Close(client *C.EtcdClient) {
	if client == nil {
		return
	}

	h := cgo.Handle(uintptr(unsafe.Pointer(client)))
	wrapper, ok := h.Value().(*etcdWrapper)
	if !ok {
		return
	}
	wrapper.mu.Lock()
	lastErr := wrapper.lastErrC
	wrapper.lastErrC = nil
	session := wrapper.session
	mutex := wrapper.mutex
	wrapper.session = nil
	wrapper.mutex = nil
	keepAliveCancels := wrapper.keepAliveCancels
	wrapper.keepAliveCancels = nil
	cli := wrapper.client
	wrapper.client = nil
	wrapper.mu.Unlock()
	if lastErr != nil {
		C.free(unsafe.Pointer(lastErr))
	}

	// Cancel all keepalive goroutines from TTL Puts
	for _, cancel := range keepAliveCancels {
		cancel()
	}

	if mutex != nil {
		ctx, cancel := context.WithTimeout(context.Background(), CleanupTimeout)
		_ = mutex.Unlock(ctx)
		cancel()
	}

	if session != nil {
		_ = session.Close()
	}

	if cli != nil {
		_ = cli.Close()
	}

	h.Delete()
}

//export Etcd_GetLastError
func Etcd_GetLastError(client *C.EtcdClient) *C.char {
	w := getWrapper(client)
	if w == nil {
		return nil
	}

	w.mu.Lock()
	defer w.mu.Unlock()
	// NOTE: Returns interior pointer owned by the wrapper.
	// Caller must not free it. Valid until next API call on this client.
	return w.lastErrC
}

//export Etcd_Put
func Etcd_Put(client *C.EtcdClient, key *C.char, value unsafe.Pointer, valueLen C.size_t, ttlSeconds C.int64_t) C.int {
	w := getWrapper(client)
	if w == nil {
		return -1
	}

	if key == nil {
		w.setError(fmt.Errorf("key is nil"))
		return -1
	}

	goKey := C.GoString(key)
	var valStr string
	if value != nil && valueLen > 0 {
		valStr = string(unsafe.Slice((*byte)(value), int(valueLen)))
	}

	ctx, cancel := context.WithTimeout(context.Background(), w.timeout)
	defer cancel()

	var err error
	var opts []clientv3.OpOption

	if ttlSeconds > 0 {
		var leaseResp *clientv3.LeaseGrantResponse
		leaseResp, err = w.client.Grant(ctx, int64(ttlSeconds))
		if err != nil {
			w.setError(fmt.Errorf("lease grant failed: %w", err))
			return -1
		}

		kaCtx, kaCancel := context.WithCancel(context.Background())

		// Register this keepalive independently in the map
		w.mu.Lock()
		if w.keepAliveCancels == nil {
			w.keepAliveCancels = make(map[clientv3.LeaseID]context.CancelFunc)
		}
		w.keepAliveCancels[leaseResp.ID] = kaCancel
		w.mu.Unlock()

		kaCh, kaErr := w.client.KeepAlive(kaCtx, leaseResp.ID)
		if kaErr != nil {
			fmt.Fprintf(os.Stderr, "[etcd-cgo] KeepAlive failed for lease %x: %v\n", leaseResp.ID, kaErr)
			kaCancel()
		} else {
			go drainKeepAlive(kaCtx, kaCh, leaseResp.ID, w)
		}

		opts = append(opts, clientv3.WithLease(leaseResp.ID))
	}

	_, err = w.client.Put(ctx, goKey, valStr, opts...)
	if err != nil {
		w.setError(err)
		return -1
	}

	w.setError(nil)
	return 0
}

//export Etcd_Get
func Etcd_Get(client *C.EtcdClient, key *C.char, outValue **C.char, outValueLen *C.size_t) C.int {
	w := getWrapper(client)
	if w == nil {
		return -1
	}

	if key == nil {
		w.setError(fmt.Errorf("key is nil"))
		return -1
	}

	if outValue == nil || outValueLen == nil {
		w.setError(fmt.Errorf("output parameters are nil"))
		return -1
	}

	goKey := C.GoString(key)
	ctx, cancel := context.WithTimeout(context.Background(), w.timeout)
	defer cancel()

	resp, err := w.client.Get(ctx, goKey)
	if err != nil {
		w.setError(err)
		return -1
	}

	if len(resp.Kvs) == 0 {
		w.setError(fmt.Errorf("key not found"))
		return -1
	}

	valBytes := resp.Kvs[0].Value
	length := len(valBytes)

	if length == 0 {
		cBuf := (*C.char)(C.malloc(1))
		if cBuf == nil {
			w.setError(fmt.Errorf("malloc failed"))
			return -1
		}
		*cBuf = 0
		*outValue = cBuf
		*outValueLen = 0
	} else {
		*outValue = (*C.char)(C.CBytes(valBytes))
		*outValueLen = C.size_t(length)
	}

	w.setError(nil)
	return 0
}

//export Etcd_PrefixGet
func Etcd_PrefixGet(client *C.EtcdClient, cCtx *C.smem_store_prefix_get_ctx_t, _ C.int) C.int {
	w := getWrapper(client)
	if w == nil {
		return -1
	}

	if cCtx == nil || cCtx.prefix == nil {
		w.setError(fmt.Errorf("context or prefix is nil"))
		return -1
	}

	// 1. 转换参数
	goPrefix := C.GoString(cCtx.prefix)
	var startKey string
	var endKey string

	// prefix的下一个字节
	endKey = string(clientv3.GetPrefixRangeEnd(goPrefix))

	// 计算起始键
	if cCtx.marker != nil {
		markerStr := C.GoString(cCtx.marker)
		if markerStr >= goPrefix {
			startKey = markerStr
		} else {
			startKey = goPrefix
		}
	} else {
		startKey = goPrefix
	}

	getOpts := []clientv3.OpOption{
		clientv3.WithRange(endKey), // 设置结束范围
	}

	ctx, cancel := context.WithTimeout(context.Background(), w.timeout)
	defer cancel()

	resp, err := w.client.Get(ctx, startKey, getOpts...)
	if err != nil {
		w.setError(err)
		return -1
	}

	for _, kv := range resp.Kvs {
		cValue := C.CBytes(kv.Value)
		cKey := C.CString(string(kv.Key))

		if cKey == nil || cValue == nil {
			if cKey != nil {
				C.free(unsafe.Pointer(cKey))
			}
			if cValue != nil {
				C.free(cValue)
			}
			w.setError(fmt.Errorf("memory allocation failed"))
			return -1
		}

		shouldContinue := C.call_fill_callback(cCtx, cKey, cValue, C.uint64_t(len(kv.Value)), cCtx.context)

		C.free(unsafe.Pointer(cKey))
		C.free(cValue)

		if !shouldContinue {
			break
		}
	}

	w.setError(nil)
	return 0
}

//export Etcd_FreeValue
func Etcd_FreeValue(value *C.char) {
	if value != nil {
		C.free(unsafe.Pointer(value))
	}
}

//export Etcd_Remove
func Etcd_Remove(client *C.EtcdClient, key *C.char) C.int {
	w := getWrapper(client)
	if w == nil {
		return -1
	}

	if key == nil {
		w.setError(fmt.Errorf("key is nil"))
		return -1
	}

	goKey := C.GoString(key)
	ctx, cancel := context.WithTimeout(context.Background(), w.timeout)
	defer cancel()

	_, err := w.client.Delete(ctx, goKey)
	if err != nil {
		w.setError(err)
		return -1
	}

	w.setError(nil)
	return 0
}

//export Etcd_Lock
func Etcd_Lock(client *C.EtcdClient) C.int {
	return etcdLockInternal(client, DefaultLockKey)
}

//export Etcd_LockNamed
func Etcd_LockNamed(client *C.EtcdClient, lockName *C.char) C.int {
	if lockName == nil {
		w := getWrapper(client)
		if w != nil {
			w.setError(fmt.Errorf("lockName is nil"))
		}
		return -1
	}

	return etcdLockInternal(client, C.GoString(lockName))
}

func etcdLockInternal(client *C.EtcdClient, lockName string) C.int {
	w := getWrapper(client)
	if w == nil {
		return -1
	}
	if lockName == "" {
		w.setError(fmt.Errorf("lockName is empty"))
		return -1
	}

	w.mu.Lock()

	if w.mutex != nil {
		w.setErrorLocked(fmt.Errorf("lock already held by this client"))
		w.mu.Unlock()
		return -1
	}

	if w.session == nil {
		s, err := concurrency.NewSession(w.client, concurrency.WithTTL(SessionTTL))
		if err != nil {
			w.setErrorLocked(fmt.Errorf("session init failed: %w", err))
			w.mu.Unlock()
			return -1
		}
		w.session = s
	}

	// Cancel any previous keepalive goroutines from TTL Puts
	for _, cancel := range w.keepAliveCancels {
		cancel()
	}
	w.keepAliveCancels = make(map[clientv3.LeaseID]context.CancelFunc)

	mutex := concurrency.NewMutex(w.session, lockName)
	w.mutex = mutex
	w.mu.Unlock()
	// Bound the lock wait: context.Background() would block forever if a stale
	// lock key (from a killed leader) never expires, wedging the election loop.
	// On timeout, clean up the session so its lease keepalive stops and the lock
	// key does not accumulate.
	lockCtx, lockCancel := context.WithTimeout(context.Background(), LockAcquireTimeout)
	err := mutex.Lock(lockCtx)
	lockCancel()
	if err != nil {
		w.setError(err)

		// Clean up on failure
		w.mu.Lock()
		if w.session != nil {
			_ = w.session.Close()
			w.session = nil
		}
		w.mutex = nil
		w.mu.Unlock()
		return -1
	}

	w.setError(nil)
	return 0
}

//export Etcd_UnLock
func Etcd_UnLock(client *C.EtcdClient) C.int {
	w := getWrapper(client)
	if w == nil {
		return -1
	}

	w.mu.Lock()
	mutex := w.mutex
	if mutex == nil {
		w.setErrorLocked(fmt.Errorf("lock not held"))
		w.mu.Unlock()
		return -1
	}

	w.mutex = nil
	w.mu.Unlock()

	ctx, cancel := context.WithTimeout(context.Background(), w.timeout)
	err := mutex.Unlock(ctx)
	cancel()

	if err != nil {
		w.setError(err)
		return -1
	}

	w.setError(nil)
	return 0
}

func main() {}
