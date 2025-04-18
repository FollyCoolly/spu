// Copyright 2021 Ant Group Co., Ltd.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "libspu/mpc/semi2k/arithmetic.h"

#include <functional>

#include "libspu/core/type_util.h"
#include "libspu/core/vectorize.h"
#include "libspu/mpc/api.h"
#include "libspu/mpc/common/communicator.h"
#include "libspu/mpc/common/prg_state.h"
#include "libspu/mpc/common/pv2k.h"
#include "libspu/mpc/semi2k/state.h"
#include "libspu/mpc/semi2k/type.h"
#include "libspu/mpc/utils/ring_ops.h"

namespace spu::mpc::semi2k {

NdArrayRef RandA::proc(KernelEvalContext* ctx, const Shape& shape) const {
  auto* prg_state = ctx->getState<PrgState>();
  const auto field = ctx->getState<Z2kState>()->getDefaultField();

  // NOTES for ring_rshift to 2 bits.
  // Refer to:
  // New Primitives for Actively-Secure MPC over Rings with Applications to
  // Private Machine Learning
  // - https://eprint.iacr.org/2019/599.pdf
  // It's safer to keep the number within [-2**(k-2), 2**(k-2)) for comparison
  // operations.
  return ring_rshift(prg_state->genPriv(field, shape), {2})
      .as(makeType<AShrTy>(field));
}

NdArrayRef P2A::proc(KernelEvalContext* ctx, const NdArrayRef& in) const {
  const auto field = in.eltype().as<Ring2k>()->field();
  auto* prg_state = ctx->getState<PrgState>();
  auto* comm = ctx->getState<Communicator>();

  auto [r0, r1] =
      prg_state->genPrssPair(field, in.shape(), PrgState::GenPrssCtrl::Both);
  auto x = ring_sub(r0, r1).as(makeType<AShrTy>(field));

  if (comm->getRank() == 0) {
    ring_add_(x, in);
  }

  return x.as(makeType<AShrTy>(field));
}

NdArrayRef A2P::proc(KernelEvalContext* ctx, const NdArrayRef& in) const {
  const auto field = in.eltype().as<Ring2k>()->field();
  auto* comm = ctx->getState<Communicator>();
  auto out = comm->allReduce(ReduceOp::ADD, in, kBindName());
  return out.as(makeType<Pub2kTy>(field));
}

NdArrayRef A2V::proc(KernelEvalContext* ctx, const NdArrayRef& in,
                     size_t rank) const {
  auto* comm = ctx->getState<Communicator>();
  const auto field = in.eltype().as<AShrTy>()->field();
  auto out_ty = makeType<Priv2kTy>(field, rank);

  auto numel = in.numel();

  return DISPATCH_ALL_FIELDS(field, [&]() {
    std::vector<ring2k_t> share(numel);
    NdArrayView<ring2k_t> _in(in);
    pforeach(0, numel, [&](int64_t idx) { share[idx] = _in[idx]; });

    std::vector<std::vector<ring2k_t>> shares =
        comm->gather<ring2k_t>(share, rank, "a2v");  // comm => 1, k
    if (comm->getRank() == rank) {
      SPU_ENFORCE(shares.size() == comm->getWorldSize());
      NdArrayRef out(out_ty, in.shape());
      NdArrayView<ring2k_t> _out(out);
      pforeach(0, numel, [&](int64_t idx) {
        ring2k_t s = 0;
        for (auto& share : shares) {
          s += share[idx];
        }
        _out[idx] = s;
      });
      return out;
    } else {
      return makeConstantArrayRef(out_ty, in.shape());
    }
  });
}

NdArrayRef V2A::proc(KernelEvalContext* ctx, const NdArrayRef& in) const {
  const auto* in_ty = in.eltype().as<Priv2kTy>();
  const size_t owner_rank = in_ty->owner();
  const auto field = in_ty->field();
  auto* prg_state = ctx->getState<PrgState>();
  auto* comm = ctx->getState<Communicator>();

  auto [r0, r1] =
      prg_state->genPrssPair(field, in.shape(), PrgState::GenPrssCtrl::Both);
  auto x = ring_sub(r0, r1).as(makeType<AShrTy>(field));

  if (comm->getRank() == owner_rank) {
    ring_add_(x, in);
  }

  return x.as(makeType<AShrTy>(field));
}

NdArrayRef NegateA::proc(KernelEvalContext* ctx, const NdArrayRef& in) const {
  auto res = ring_neg(in);
  return res.as(in.eltype());
}

////////////////////////////////////////////////////////////////////
// add family
////////////////////////////////////////////////////////////////////
NdArrayRef AddAP::proc(KernelEvalContext* ctx, const NdArrayRef& lhs,
                       const NdArrayRef& rhs) const {
  SPU_ENFORCE(lhs.numel() == rhs.numel());
  auto* comm = ctx->getState<Communicator>();

  if (comm->getRank() == 0) {
    return ring_add(lhs, rhs).as(lhs.eltype());
  }

  return lhs;
}

NdArrayRef AddAA::proc(KernelEvalContext*, const NdArrayRef& lhs,
                       const NdArrayRef& rhs) const {
  SPU_ENFORCE(lhs.numel() == rhs.numel());
  SPU_ENFORCE(lhs.eltype() == rhs.eltype());

  return ring_add(lhs, rhs).as(lhs.eltype());
}

////////////////////////////////////////////////////////////////////
// multiply family
////////////////////////////////////////////////////////////////////
NdArrayRef MulAP::proc(KernelEvalContext*, const NdArrayRef& lhs,
                       const NdArrayRef& rhs) const {
  return ring_mul(lhs, rhs).as(lhs.eltype());
}

namespace {

NdArrayRef UnflattenBuffer(yacl::Buffer&& buf, const Type& t, const Shape& s) {
  return NdArrayRef(std::make_shared<yacl::Buffer>(std::move(buf)), t, s);
}

NdArrayRef UnflattenBuffer(yacl::Buffer&& buf, const NdArrayRef& x) {
  return NdArrayRef(std::make_shared<yacl::Buffer>(std::move(buf)), x.eltype(),
                    x.shape());
}

std::tuple<NdArrayRef, NdArrayRef, NdArrayRef, NdArrayRef, NdArrayRef> MulOpen(
    KernelEvalContext* ctx, const NdArrayRef& x, const NdArrayRef& y,
    bool mmul) {
  const auto field = x.eltype().as<Ring2k>()->field();
  auto* comm = ctx->getState<Communicator>();
  auto* beaver = ctx->getState<Semi2kState>()->beaver();
  auto* beaver_cache = ctx->getState<Semi2kState>()->beaver_cache();
  auto x_cache = beaver_cache->GetCache(x, mmul);
  auto y_cache = beaver_cache->GetCache(y, mmul);

  // can't init on same array twice
  if (x == y && x_cache.enabled && x_cache.replay_desc.status == Beaver::Init) {
    // FIXME: how to avoid open on same array twice (x.t dot x)
    y_cache.enabled = false;
  }

  Shape z_shape;
  if (mmul) {
    SPU_ENFORCE(x.shape()[1] == y.shape()[0]);
    z_shape = Shape{x.shape()[0], y.shape()[1]};
  } else {
    SPU_ENFORCE(x.shape() == y.shape());
    z_shape = x.shape();
  }

  // generate beaver multiple triple.
  NdArrayRef a;
  NdArrayRef b;
  NdArrayRef c;
  if (mmul) {
    auto [a_buf, b_buf, c_buf] =
        beaver->Dot(field, x.shape()[0], y.shape()[1], x.shape()[1],  //
                    x_cache.enabled ? &x_cache.replay_desc : nullptr,
                    y_cache.enabled ? &y_cache.replay_desc : nullptr);
    SPU_ENFORCE(static_cast<size_t>(a_buf.size()) == x.numel() * SizeOf(field));
    SPU_ENFORCE(static_cast<size_t>(b_buf.size()) == y.numel() * SizeOf(field));
    SPU_ENFORCE(static_cast<size_t>(c_buf.size()) ==
                z_shape.numel() * SizeOf(field));

    a = UnflattenBuffer(std::move(a_buf), x);
    b = UnflattenBuffer(std::move(b_buf), y);
    c = UnflattenBuffer(std::move(c_buf), x.eltype(), z_shape);
  } else {
    const size_t numel = x.shape().numel();
    auto [a_buf, b_buf, c_buf] =
        beaver->Mul(field, numel,  //
                    x_cache.enabled ? &x_cache.replay_desc : nullptr,
                    y_cache.enabled ? &y_cache.replay_desc : nullptr);
    SPU_ENFORCE(static_cast<size_t>(a_buf.size()) == numel * SizeOf(field));
    SPU_ENFORCE(static_cast<size_t>(b_buf.size()) == numel * SizeOf(field));
    SPU_ENFORCE(static_cast<size_t>(c_buf.size()) == numel * SizeOf(field));

    a = UnflattenBuffer(std::move(a_buf), x);
    b = UnflattenBuffer(std::move(b_buf), y);
    c = UnflattenBuffer(std::move(c_buf), x);
  }

  // Open x-a & y-b
  NdArrayRef x_a;
  NdArrayRef y_b;

  auto x_hit_cache = x_cache.replay_desc.status != Beaver::Init;
  auto y_hit_cache = y_cache.replay_desc.status != Beaver::Init;

  if (ctx->sctx()->config().experimental_disable_vectorization || x_hit_cache ||
      y_hit_cache) {
    if (x_hit_cache) {
      x_a = std::move(x_cache.open_cache);
    } else {
      x_a = comm->allReduce(ReduceOp::ADD, ring_sub(x, a), "open(x-a)");
    }
    if (y_hit_cache) {
      y_b = std::move(y_cache.open_cache);
    } else {
      y_b = comm->allReduce(ReduceOp::ADD, ring_sub(y, b), "open(y-b)");
    }
  } else {
    auto res = vmap({ring_sub(x, a), ring_sub(y, b)}, [&](const NdArrayRef& s) {
      return comm->allReduce(ReduceOp::ADD, s, "open(x-a,y-b)");
    });
    x_a = std::move(res[0]);
    y_b = std::move(res[1]);
  }

  if (x_cache.enabled && x_cache.replay_desc.status == Beaver::Init) {
    beaver_cache->SetCache(x, x_cache.replay_desc, x_a);
  }
  if (y_cache.enabled && y_cache.replay_desc.status == Beaver::Init) {
    beaver_cache->SetCache(y, y_cache.replay_desc, y_b);
  }

  return {std::move(a), std::move(b), std::move(c), std::move(x_a),
          std::move(y_b)};
}

}  // namespace

NdArrayRef MulAA::proc(KernelEvalContext* ctx, const NdArrayRef& x,
                       const NdArrayRef& y) const {
  auto* comm = ctx->getState<Communicator>();

  auto [a, b, c, x_a, y_b] = MulOpen(ctx, x, y, false);

  // Zi = Ci + (X - A) * Bi + (Y - B) * Ai + <(X - A) * (Y - B)>
  ring_mul_(b, x_a);
  ring_mul_(a, y_b);
  ring_add_(b, a);
  ring_add_(b, c);

  if (comm->getRank() == 0) {
    // z += (X-A) * (Y-B);
    ring_mul_(x_a, y_b);
    ring_add_(b, x_a);
  }
  return b.as(x.eltype());
}

NdArrayRef SquareA::proc(KernelEvalContext* ctx, const NdArrayRef& x) const {
  const auto field = x.eltype().as<Ring2k>()->field();
  auto* comm = ctx->getState<Communicator>();
  auto* beaver = ctx->getState<Semi2kState>()->beaver();
  auto* beaver_cache = ctx->getState<Semi2kState>()->beaver_cache();
  auto x_cache = beaver_cache->GetCache(x, false);

  // generate beaver Square pair.
  NdArrayRef a;
  NdArrayRef b;
  const size_t numel = x.shape().numel();
  auto [a_buf, b_buf] =
      beaver->Square(field, numel,  //
                     x_cache.enabled ? &x_cache.replay_desc : nullptr);
  SPU_ENFORCE(static_cast<size_t>(a_buf.size()) == numel * SizeOf(field));
  SPU_ENFORCE(static_cast<size_t>(b_buf.size()) == numel * SizeOf(field));

  a = UnflattenBuffer(std::move(a_buf), x);
  b = UnflattenBuffer(std::move(b_buf), x);

  // Open x-a
  NdArrayRef x_a;

  if (x_cache.replay_desc.status != Beaver::Init) {
    x_a = std::move(x_cache.open_cache);
  } else {
    x_a = comm->allReduce(ReduceOp::ADD, ring_sub(x, a), "open(x-a)");
  }

  if (x_cache.enabled && x_cache.replay_desc.status == Beaver::Init) {
    beaver_cache->SetCache(x, x_cache.replay_desc, x_a);
  }

  // Zi = Bi + 2 * (X - A) * Ai + <(X - A) * (X - A)>
  auto z = ring_add(ring_mul(ring_mul(std::move(a), x_a), 2), b);
  if (comm->getRank() == 0) {
    // z += (X - A) * (X - A);
    ring_add_(z, ring_mul(x_a, x_a));
  }
  return z.as(x.eltype());
}

// Let x be AShrTy, y be BShrTy, nbits(y) == 1
// (x0+x1) * (y0^y1) = (x0+x1) * (y0+y1-2y0y1)
// we define xx0 = (1-2y0)x0, xx1 = (1-2y1)x1
//           yy0 = y0,        yy1 = y1
// if we can compute z0+z1 = xx0*yy1 + xx1*yy0 (which can be easily got from Mul
// Beaver), then (x0+x1) * (y0^y1) = (z0 + z1) + (x0y0 + x1y1)
NdArrayRef MulA1B::proc(KernelEvalContext* ctx, const NdArrayRef& x,
                        const NdArrayRef& y) const {
  SPU_ENFORCE(x.eltype().as<RingTy>()->field() ==
              y.eltype().as<RingTy>()->field());

  const auto field = x.eltype().as<RingTy>()->field();
  auto* comm = ctx->getState<Communicator>();

  // IMPORTANT: the underlying value of y is not exactly 0 or 1, so we must mask
  // it explicitly.
  auto yy = ring_bitmask(y, 0, 1).as(makeType<RingTy>(field));
  // To optimize memory usage, re-use xx buffer
  auto xx = ring_ones(field, x.shape());
  ring_sub_(xx, ring_lshift(yy, {1}));
  ring_mul_(xx, x);

  auto [a, b, c, xx_a, yy_b] = MulOpen(ctx, xx, yy, false);

  // Zi = Ci + (XX - A) * Bi + (YY - B) * Ai + <(XX - A) * (YY - B)> - XXi * YYi
  // We re-use b to compute z
  ring_mul_(b, xx_a);
  ring_mul_(a, yy_b);
  ring_add_(b, a);
  ring_add_(b, c);

  ring_mul_(xx, yy);
  ring_sub_(b, xx);
  if (comm->getRank() == 0) {
    // z += (XX-A) * (YY-B);
    ring_mul_(xx_a, yy_b);
    ring_add_(b, xx_a);
  }

  // zi += xi * yi
  ring_add_(b, ring_mul(x, yy));

  return b.as(x.eltype());
}

namespace {
NdArrayRef UnflattenBuffer(yacl::Buffer&& buf, FieldType field,
                           const Shape& shape) {
  return NdArrayRef(std::make_shared<yacl::Buffer>(std::move(buf)),
                    makeType<RingTy>(field), shape);
}
}  // namespace

// Input: P0 has x, P1 has y;
// Output: P0 has z0, P1 has z1, where z0 + z1 = x * y
// Steps:
//   1. Beaver generate & send (a0,c0), (a1,c1), where a0 * a1 = c0 + c1
//   2. P0 send (x+a0), P1 send (y+a1) to each other
//   3. P0 compute z0 =  x(y+a1)  + c0
//      P1 compute z1 = -a1(x+a0) + c1
NdArrayRef MulVVS::proc(KernelEvalContext* ctx, const NdArrayRef& x,
                        const NdArrayRef& y) const {
  const auto x_rank = x.eltype().as<Priv2kTy>()->owner();
  const auto y_rank = y.eltype().as<Priv2kTy>()->owner();
  SPU_ENFORCE_NE(x_rank, y_rank);

  const auto field = x.eltype().as<Ring2k>()->field();
  auto* comm = ctx->getState<Communicator>();
  auto* beaver = ctx->getState<Semi2kState>()->beaver();
  const auto numel = x.shape().numel();
  const int64_t rank = comm->getRank();

  NdArrayRef in;
  if (rank == x_rank) {
    in = x;
  } else if (rank == y_rank) {
    in = y;
  } else {
    SPU_THROW("Invalid rank: {}", rank);
  }

  NdArrayRef a;
  NdArrayRef c;

  // We need private bit mul (x * y) in the smaller ring (over `bits` bits)
  // we have a0 * a1 = c0 + c1
  auto [a_buf, c_buf] = beaver->MulPriv(field, numel, ElementType::kRing);
  SPU_ENFORCE(static_cast<size_t>(a_buf.size()) == numel * SizeOf(field));
  SPU_ENFORCE(static_cast<size_t>(c_buf.size()) == numel * SizeOf(field));

  a = UnflattenBuffer(std::move(a_buf), field, x.shape());
  c = UnflattenBuffer(std::move(c_buf), field, x.shape());

  auto a_x = ring_add(a, in);
  comm->sendAsync(comm->nextRank(), a_x, "a0+x_or_a1+y");
  auto tmp =
      comm->recv(comm->prevRank(), makeType<AShrTy>(field), "a0+x_or_a1+y")
          .reshape(in.shape());
  comm->addCommStatsManually(1, SizeOf(field) * 8 * numel);

  if (rank == 0) {
    ring_mul_(tmp, in);
    ring_add_(tmp, c);
  } else if (rank == 1) {
    ring_neg_(a);
    ring_mul_(tmp, a);
    ring_add_(tmp, c);
  } else {
    SPU_THROW("Invalid rank: {}", rank);
  }

  return tmp;
}

////////////////////////////////////////////////////////////////////
// matmul family
////////////////////////////////////////////////////////////////////
NdArrayRef MatMulAP::proc(KernelEvalContext*, const NdArrayRef& x,
                          const NdArrayRef& y) const {
  return ring_mmul(x, y).as(x.eltype());
}

NdArrayRef MatMulAA::proc(KernelEvalContext* ctx, const NdArrayRef& x,
                          const NdArrayRef& y) const {
  auto* comm = ctx->getState<Communicator>();

  auto [a, b, c, x_a, y_b] = MulOpen(ctx, x, y, true);

  // Zi = Ci + (X - A) dot Bi + Ai dot (Y - B) + <(X - A) dot (Y - B)>
  auto z = ring_add(ring_add(ring_mmul(x_a, b), ring_mmul(a, y_b)), c);
  if (comm->getRank() == 0) {
    // z += (X-A) * (Y-B);
    ring_add_(z, ring_mmul(x_a, y_b));
  }
  return z.as(x.eltype());
}

NdArrayRef LShiftA::proc(KernelEvalContext*, const NdArrayRef& in,
                         const Sizes& bits) const {
  return ring_lshift(in, bits).as(in.eltype());
}

NdArrayRef TruncA::proc(KernelEvalContext* ctx, const NdArrayRef& x,
                        size_t bits, SignType sign) const {
  auto* comm = ctx->getState<Communicator>();

  (void)sign;  // TODO: optimize me.

  // TODO: add truncation method to options.
  if (comm->getWorldSize() == 2) {
    // SecureML, local truncation.
    // Ref: Theorem 1. https://eprint.iacr.org/2017/396.pdf
    return ring_arshift(x, {static_cast<int64_t>(bits)}).as(x.eltype());
  } else {
    // ABY3, truncation pair method.
    // Ref: Section 5.1.2 https://eprint.iacr.org/2018/403.pdf
    auto* beaver = ctx->getState<Semi2kState>()->beaver();

    const auto field = x.eltype().as<Ring2k>()->field();
    auto [r_buf, rb_buf] = beaver->Trunc(field, x.shape().numel(), bits);

    NdArrayRef r(std::make_shared<yacl::Buffer>(std::move(r_buf)), x.eltype(),
                 x.shape());
    NdArrayRef rb(std::make_shared<yacl::Buffer>(std::move(rb_buf)), x.eltype(),
                  x.shape());

    // open x - r
    auto x_r = comm->allReduce(ReduceOp::ADD, ring_sub(x, r), kBindName());
    auto res = rb;
    if (comm->getRank() == 0) {
      ring_add_(res, ring_arshift(x_r, {static_cast<int64_t>(bits)}));
    }

    // res = [x-r] + [r], x which [*] is truncation operation.
    return res.as(x.eltype());
  }
}

NdArrayRef TruncAPr::proc(KernelEvalContext* ctx, const NdArrayRef& in,
                          size_t bits, SignType sign) const {
  (void)sign;  // TODO: optimize me.
  auto* comm = ctx->getState<Communicator>();
  auto* beaver = ctx->getState<Semi2kState>()->beaver();
  const auto numel = in.numel();
  const auto field = in.eltype().as<Ring2k>()->field();
  const size_t k = SizeOf(field) * 8;

  NdArrayRef out(in.eltype(), in.shape());

  DISPATCH_ALL_FIELDS(field, [&]() {
    using U = ring2k_t;
    auto [r, rc, rb] = beaver->TruncPr(field, numel, bits);
    SPU_ENFORCE(static_cast<size_t>(r.size()) == numel * SizeOf(field));
    SPU_ENFORCE(static_cast<size_t>(rc.size()) == numel * SizeOf(field));
    SPU_ENFORCE(static_cast<size_t>(rb.size()) == numel * SizeOf(field));

    NdArrayView<U> _in(in);
    absl::Span<const U> _r(r.data<U>(), numel);
    absl::Span<const U> _rc(rc.data<U>(), numel);
    absl::Span<const U> _rb(rb.data<U>(), numel);
    NdArrayView<U> _out(out);

    std::vector<U> c;
    {
      std::vector<U> x_plus_r(numel);

      pforeach(0, numel, [&](int64_t idx) {
        auto x = _in[idx];
        // handle negative number.
        // assume secret x in [-2^(k-2), 2^(k-2)), by
        // adding 2^(k-2) x' = x + 2^(k-2) in [0, 2^(k-1)), with msb(x') == 0
        if (comm->getRank() == 0) {
          x += U(1) << (k - 2);
        }
        // mask x with r
        x_plus_r[idx] = x + _r[idx];
      });
      // open <x> + <r> = c
      c = comm->allReduce<U, std::plus>(x_plus_r, kBindName());
    }

    pforeach(0, numel, [&](int64_t idx) {
      auto ck_1 = c[idx] >> (k - 1);

      U y;
      if (comm->getRank() == 0) {
        // <b> = <rb> ^ c{k-1} = <rb> + c{k-1} - 2*c{k-1}*<rb>
        auto b = _rb[idx] + ck_1 - 2 * ck_1 * _rb[idx];
        // c_hat = c/2^m mod 2^(k-m-1) = (c << 1) >> (1+m)
        auto c_hat = (c[idx] << 1) >> (1 + bits);
        // y = c_hat - <rc> + <b> * 2^(k-m-1)
        y = c_hat - _rc[idx] + (b << (k - 1 - bits));
        // re-encode negative numbers.
        // from https://eprint.iacr.org/2020/338.pdf, section 5.1
        // y' = y - 2^(k-2-m)
        y -= (U(1) << (k - 2 - bits));
      } else {
        auto b = _rb[idx] + 0 - 2 * ck_1 * _rb[idx];
        y = 0 - _rc[idx] + (b << (k - 1 - bits));
      }

      _out[idx] = y;
    });
  });

  return out;
}

namespace {

static NdArrayRef wrap_mulvvs(SPUContext* ctx, const NdArrayRef& x,
                              const NdArrayRef& y) {
  SPU_ENFORCE(x.shape() == y.shape());
  SPU_ENFORCE(x.eltype().isa<Priv2kTy>());
  SPU_ENFORCE(y.eltype().isa<Priv2kTy>());
  SPU_ENFORCE(x.eltype().as<Priv2kTy>()->owner() !=
              y.eltype().as<Priv2kTy>()->owner());
  return UnwrapValue(mul_vv(ctx, WrapValue(x), WrapValue(y)));
}

// TODO: define more smaller fields.
FieldType getTruncField(size_t bits) {
  if (bits <= 32) {
    return FM32;
  } else if (bits <= 64) {
    return FM64;
  } else if (bits <= 128) {
    return FM128;
  } else {
    SPU_THROW("Unsupported truncation bits: {}", bits);
  }
}

// Ref: Improved secure two-party computation from a geometric perspective
// Algorithm 2: Compute MW(x, L) with |x| < L / 4
NdArrayRef computeMW(KernelEvalContext* ctx, const NdArrayRef& in,
                     size_t bits) {
  const auto numel = in.numel();
  const auto field = in.eltype().as<Ring2k>()->field();
  const size_t k = SizeOf(field) * 8;
  const auto trunc_field = getTruncField(bits);
  auto* comm = ctx->getState<Communicator>();

  NdArrayRef mw;

  DISPATCH_ALL_FIELDS(field, [&]() {
    using ele_t = ring2k_t;
    const ele_t L_4 = ele_t(1) << (k - TruncAPr2::kBitsLeftOut);
    const ele_t L_2 = L_4 << 1;

    DISPATCH_ALL_FIELDS(trunc_field, [&]() {
      using mw_t = ring2k_t;

      const auto trunc_ty = makeType<RingTy>(trunc_field);
      NdArrayRef in_star(trunc_ty, in.shape());

      NdArrayView<ele_t> _in(in);
      NdArrayView<mw_t> _in_star(in_star);
      if (comm->getRank() == 0) {
        pforeach(0, numel, [&](int64_t idx) {  //
          _in_star[idx] = static_cast<mw_t>((_in[idx] - L_4) >= L_2);
        });
      } else if (comm->getRank() == 1) {
        pforeach(0, numel, [&](int64_t idx) {  //
          _in_star[idx] = static_cast<mw_t>(_in[idx] >= L_2);
        });
      } else {
        SPU_THROW("Invalid rank: {}", comm->getRank());
      }

      NdArrayRef x;
      NdArrayRef y;
      const auto pri0_ty = makeType<Priv2kTy>(trunc_field, 0);
      const auto pri1_ty = makeType<Priv2kTy>(trunc_field, 1);
      if (comm->getRank() == 0) {
        x = in_star.as(pri0_ty);
        y = makeConstantArrayRef(pri1_ty, in_star.shape());
      } else {
        x = makeConstantArrayRef(pri0_ty, in_star.shape());
        y = in_star.as(pri1_ty);
      }

      mw = wrap_mulvvs(ctx->sctx(), x, y);

      NdArrayView<mw_t> _mw(mw);
      if (comm->getRank() == 0) {
        pforeach(0, numel, [&](int64_t idx) {  //
          _mw[idx] +=
              (static_cast<mw_t>(1) - static_cast<mw_t>(_in[idx] < L_4));
        });
      }
    });
  });

  return mw.as(makeType<AShrTy>(trunc_field));
}
}  // namespace

// Ref: Improved secure two-party computation from a geometric perspective
// Algorithm 4: One-bit error truncation with constraint
NdArrayRef TruncAPr2::proc(KernelEvalContext* ctx, const NdArrayRef& in,
                           size_t bits, SignType sign) const {
  (void)sign;

  const auto numel = in.numel();
  const auto field = in.eltype().as<Ring2k>()->field();
  const size_t k = SizeOf(field) * 8;
  const auto trunc_field = getTruncField(bits);
  auto* comm = ctx->getState<Communicator>();
  const auto rank = comm->getRank();
  SPU_ENFORCE(rank == 0 || rank == 1, "Invalid rank: {}", rank);

  // MW(x0, x1, L) = Wrap(x0, x1, L) + MSB(x)
  auto mw = computeMW(ctx, in, bits);

  NdArrayRef out(in.eltype(), in.shape());

  DISPATCH_ALL_FIELDS(field, [&]() {
    using ele_t = ring2k_t;

    DISPATCH_ALL_FIELDS(trunc_field, [&]() {
      using mw_t = ring2k_t;
      // (x >> k) = (x0 >> k) + (x1 >> k) - MW(x) * (2^{l-k}) + 1, with
      // one-bit error at most.
      // Note: we choose to add 1 rather ignore it, because we want to make
      // trunc(0, fxp_bits) = 0, else the result will be -2**{-fxp_bits},
      // which may cause some confusion.
      NdArrayView<mw_t> _mw(mw);
      NdArrayView<ele_t> _in(in);
      NdArrayView<ele_t> _out(out);

      pforeach(0, numel, [&](int64_t idx) {
        _out[idx] = (_in[idx] >> bits) -
                    static_cast<ele_t>(_mw[idx]) *
                        (static_cast<ele_t>(1) << (k - bits)) +
                    rank;
      });
    });
  });

  return out;
}

void BeaverCacheKernel::evaluate(KernelEvalContext* ctx) const {
  const auto& v = ctx->getParam<Value>(0);
  const auto& enable_cache = ctx->getParam<bool>(1);

  auto* beaver_cache = ctx->getState<Semi2kState>()->beaver_cache();

  if (enable_cache) {
    beaver_cache->EnableCache(v.data());
    if (v.isComplex()) {
      beaver_cache->EnableCache(v.imag().value());
    }
  } else {
    beaver_cache->DisableCache(v.data());
    if (v.isComplex()) {
      beaver_cache->DisableCache(v.imag().value());
    }
  }
  // dummy output
  ctx->pushOutput(Value());
}

}  // namespace spu::mpc::semi2k
