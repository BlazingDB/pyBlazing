import asyncio
import pytest

import ucp
import cudf

from dask_cuda import LocalCUDACluster
from distributed import Client, Worker, Scheduler, wait, get_worker
from distributed.comm import ucx, listen, connect
from ..apiv2.comms import listen, BlazingMessage, UCX, register_serialization
from distributed.utils_test import cleanup  # noqa: 401

enable_tcp_over_ucx = True
enable_nvlink = False
enable_infiniband = False


try:
    HOST = ucp.get_address()
except Exception:
    HOST = "127.0.0.1"


async def mock_msg_callback(msg):

    """
    Mocks the real callback which gets invoked each time
    a message is received on an endpoint.
    """

    print(msg)

    print("Invoked w/ %s" % msg)
    if not hasattr(get_worker(), "_test_msgs_received"):
        get_worker()._test_msgs_received = []

    print("Setting callback")
    get_worker()._test_msgs_received.append(msg)


class PyBlazingCache():
    """
    Probably want to add a
    """
    def add_to_cache_with_meta(self,cudf_data,metadata):
        pass

    def add_to_cache(self,cudf_data):
        pass


@pytest.mark.asyncio
async def test_ucx_localcluster( cleanup):
    async with LocalCUDACluster(
        protocol="ucx",

        dashboard_address=None,
        n_workers=2,
        threads_per_worker=1,
        processes=True,
        asynchronous=True,
        enable_tcp_over_ucx=enable_tcp_over_ucx,
        enable_nvlink=enable_nvlink,
        enable_infiniband=enable_infiniband
    ) as cluster:
        async with Client(cluster, asynchronous=True) as client:

            """
            First need to register BlazingMessage in the serialization layer
            on Dask client and workers
            """
            register_serialization()
            await client.run(register_serialization, wait=True)

            """
            Next, simply call list using an asynchronous Dask client.
            The callback function is pushed to the workers and 
            invoked when a message is received with a BlazingMessage
            """
            ips_ports = await listen(mock_msg_callback, client)

            "<<<<<<<<<< Begin Test Logic >>>>>>>>>>>>"

            assert len(ips_ports) == len(client.scheduler_info()["workers"])
            for k, v in ips_ports.items():
                assert v is not None


            """
            Loop through each of the workers, sending a test BlazingMessage
            to all other workers.
            """
            for dask_addr, blazing_addr in ips_ports.items():
                meta = {"worker_ids": list(ips_ports.values())}
                data = cudf.DataFrame()
                data["a"] = cudf.Series([0, 1, 2, 3, 4])
                msg = BlazingMessage(meta, data)

                async def send(msg): await UCX.get().send(msg)
                await client.run(send, msg, workers=[dask_addr], wait=True)

            """
            Gather messages received on each worker for validation
            """
            received = await client.run(lambda: get_worker()._test_msgs_received, wait=True)

            print("MSGS: " + str(received))

            for worker_addr, msgs in received.items():
                assert len(msgs) == len(ips_ports)
