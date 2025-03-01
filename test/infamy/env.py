import argparse
import networkx
import os
import pydot
import shlex
import sys

from . import neigh, netconf, tap, topology

class ArgumentParser(argparse.ArgumentParser):
    def __init__(self, ltop):
        super().__init__()

        self.add_argument("-d", "--debug", default=False, action="store_true")
        self.add_argument("-l", "--logical-topology", dest="ltop", default=ltop)
        self.add_argument("-y", "--yangdir", default=None)
        self.add_argument("ptop", nargs=1, metavar="topology")


class Env(object):
    def __init__(self, ltop=None, argv=sys.argv[1::], environ=os.environ):
        if "INFAMY_ARGS" in environ:
            argv = shlex.split(environ["INFAMY_ARGS"]) + argv

        self.args = ArgumentParser(ltop).parse_args(argv)

        pdot = pydot.graph_from_dot_file(self.args.ptop[0])[0]
        self.ptop = topology.Topology(pdot)

        if self.args.ltop:
            ldot = pydot.graph_from_dot_file(self.args.ltop)[0]
            self.ltop = topology.Topology(ldot)
            if not self.ltop.map_to(self.ptop):
                raise tap.TestSkip()

    def attach(self, node, port):
        if self.ltop:
            mapping = self.ltop.mapping[node]
            node, port = self.ltop.xlate(node, port)
        else:
            mapping = None

        ctrl = self.ptop.get_ctrl()
        _, cport = self.ptop.get_path(ctrl, (node, port))[0]

        print(f"Probing {node} on port {cport} for IPv6LL mgmt address ...")
        mgmtip = neigh.ll6ping(cport)
        if not mgmtip:
            raise Exception(f"Failed, cannot find mgmt IP for {node}")

        return netconf.Device(
            location=netconf.Location(mgmtip),
            mapping=mapping,
            yangdir=self.args.yangdir
        )
