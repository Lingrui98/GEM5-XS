# Tasks: tail-halfword control-PC view for decoupled BPU

## 1. Spec and proposal updates

- [ ] Add the new OpenSpec change `update-bpu-control-pc-tail-halfword`.
- [ ] Record that it supersedes the pending semantic goal of `update-decoupled-btb-control-pc-views`.
- [ ] Add delta specs for `decoupled-btb-control-pc-semantics`.
- [ ] Update dependent pending changes to reference the new `startPC` / `controlPC` split:
  - [ ] `add-btb-entry-prefetch`
  - [ ] `add-fdip-icache-prefetch`

## 2. Predictor-side PC view refactor

- [ ] Make predictor-visible branch identity explicit:
  - [ ] `pc` means `controlPC`
  - [ ] `startPC` is stored explicitly when byte-coverage / logging still need instruction-start view
- [ ] Update BPU internal users of branch PC to use `controlPC` consistently:
  - [ ] BTB/TAGE/UBTB/MBTB/ABTB/MGSC lookup and update paths
  - [ ] tag/index/position derivation
  - [ ] taken-slot ownership and redirect matching
- [ ] Ensure split 4B control instructions are attached to the block containing `controlPC`, not the block containing `startPC`.

## 3. Fetch / decoder contract changes

- [ ] Remove the fetch-side assumption that every RISC-V decode attempt requires 4 contiguous bytes to be resident.
- [ ] Upgrade the RISC-V decoder to support single-instruction halfword-granular fill:
  - [ ] one 2B fill is enough for RVC
  - [ ] 4B RVI remains not-ready after the first 2B and requests the trailing 2B
- [ ] Make fetch feed bytes incrementally for the same instruction until decoder reports ready.
- [ ] Keep cross-boundary 4B control handling generic:
  - [ ] first block may supply the leading halfword
  - [ ] second block supplies the trailing halfword
  - [ ] no special fetch-only redirect patch for this case

## 4. Validation

- [ ] Reuse the validation framework from `update-decoupled-btb-control-pc-views`:
  - [ ] keep the same CSV manifest / env-template contract
  - [ ] keep the same build smoke / FS checkpoint / trace regression layers
  - [ ] keep runtime witness capture
- [ ] Add or rewrite directed cases for the new semantics:
  - [ ] `Rvc2B_ControlPCView`
  - [ ] `Rvi4B_ControlPCView`
  - [ ] `Rvi4B_ControlPC_InSingleBlock`
  - [ ] `Rvi4B_ControlPC_CrossBoundaryPredictInNextBlock`
  - [ ] `Decoder_Incremental2BFill_Rvi4B`
- [ ] Run `openspec validate update-bpu-control-pc-tail-halfword --strict`.
