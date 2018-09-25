# coding: utf-8

import chainer
import chainer.links as L

# Network definition


class A(chainer.Chain):

    def __init__(self, n_out):
        super(A, self).__init__()
        with self.init_scope():
            self.l1 = L.Linear(None, n_out)
            self.l2 = L.Linear(7, nobias=True)

    def forward(self, x):
        y1 = self.l1(x)
        y2 = self.l2(x)
        return (y1, y2)


# ======================================

import ch2o

if __name__ == '__main__':
    import numpy as np
    np.random.seed(314)

    model = A(3)

    x = np.random.rand(5, 7).astype(np.float32)
    x = [x]
    ch2o.generate_testcase(model, x)