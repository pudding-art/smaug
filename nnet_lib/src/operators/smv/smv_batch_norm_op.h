#ifndef _OPERATORS_SMV_SMV_BATCH_NORM_OP_H_
#define _OPERATORS_SMV_SMV_BATCH_NORM_OP_H_

#include "core/backend.h"
#include "operators/common.h"
#include "operators/batch_norm_op.h"

namespace smaug {

namespace smv {
namespace bn {

extern const int kVectorSize;

class TilingOptimizer;

}  // namespace bn
}  // namespace smv

class SmvBatchNormOp : public BatchNormOp<SmvBackend> {
  public:
    using BatchNormOp<SmvBackend>::BatchNormOp;
    virtual void run();
    virtual DataLayoutSet getInputDataLayouts() const {
        if (inputs[Inputs]->ndims() == 4)
            return DataLayoutSet(DataLayout::NCHW);
        else
            return DataLayoutSet(DataLayout::NC);
    }
    virtual DataLayoutSet getOutputDataLayouts() const {
        if (inputs[Inputs]->ndims() == 4)
            return DataLayoutSet(DataLayout::NCHW);
        return DataLayoutSet(DataLayout::NC);
    }

  protected:
   // This is for post-FC batch norm.
   void runNA(TiledTensor& inputs, TiledTensor& weights, TiledTensor& outputs);
   // This is for post-Conv bath norm.
   void runNCW(TiledTensor& inputs, TiledTensor& weights, TiledTensor& outputs);
};

}  // namespace smaug

#endif