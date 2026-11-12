#pragma once
#include "support/binaryFile.hpp"
#include "banks/Mpb.hpp"
#include "banks/RamLayout.hpp"
#include <map>
#include <utility>

namespace manatee {

class BankSource {
    public:
        enum class Mode { None, LooseMpbs, MltBundle };

        Mode GetMode() const { return sourceMode; }
        bool Empty() const { return sourceMode == Mode::None; }
        bool IsLoose() const { return sourceMode == Mode::LooseMpbs; }
        const auto &Mpbs() const { return mpbPaths; }
        const auto &Mlt() const { return mltPath; }
        const auto &Fpb() const { return fpbPath; }
        const auto &Fob() const { return fobPath; }
        const std::filesystem::path *MpbPath(unsigned slot) const {
            auto it = mpbPaths.find(slot);
            return it == mpbPaths.end() ? nullptr : &it->second;
        }

        void OpenMpbs(std::vector<std::filesystem::path> paths) {
            if (paths.empty() || paths.size() > 16)
                throw std::runtime_error("Choose 1 to 16 MPB files.");
            Clear();
            sourceMode = Mode::LooseMpbs;
            for (unsigned slot = 0; slot < paths.size(); ++slot)
                mpbPaths.emplace(slot, std::move(paths[slot]));
        }
        void OpenFile(const std::filesystem::path &path) {
            const auto bytes = readFile(path);
            if (signature(bytes, "SMLT"))
                OpenMlt(path);
            else if (isMpb(bytes))
                OpenMpbs({path});
            else
                throw std::runtime_error("Expected an MPB bank or MLT.");
        }
        void OpenMlt(std::filesystem::path path) {
            if (path.empty())
                throw std::runtime_error("Choose an MLT.");
            Clear();
            sourceMode = Mode::MltBundle;
            mltPath = std::move(path);
        }
        void SetFpb(std::filesystem::path path) {
            RequireLoose();
            fpbPath = std::move(path);
        }
        void SetFob(std::filesystem::path path) {
            RequireLoose();
            fobPath = std::move(path);
        }
        void ClearEffects() {
            RequireLoose();
            fpbPath.clear();
            fobPath.clear();
        }
        void ReplaceMpb(unsigned slot, std::filesystem::path path) {
            if (Empty())
                throw std::runtime_error("Open MPB banks or an MLT first.");
            if (slot >= 16)
                throw std::runtime_error("MPB slot out of range.");
            if (IsLoose())
                mpbPaths.at(slot) = std::move(path);
            else
                mpbPaths.insert_or_assign(slot, std::move(path));
        }
        RamLayout MakeLayout() const {
            if (sourceMode == Mode::MltBundle) {
                auto layout = RamLayout::FromMlt(readFile(mltPath));
                for (const auto &[slot, path] : mpbPaths)
                    layout.ReplaceMpb(slot, readFile(path));
                return layout;
            }
            RequireLoose();
            std::vector<Bytes> banks;
            for (const auto &[slot, path] : mpbPaths)
                banks.push_back(readFile(path));
            return RamLayout::FromBanks(banks, fpbPath.empty() ? Bytes{} : readFile(fpbPath), fobPath.empty() ? Bytes{} : readFile(fobPath));
        }

    private:
        Mode sourceMode = Mode::None;
        std::map<unsigned, std::filesystem::path> mpbPaths;
        std::filesystem::path mltPath, fpbPath, fobPath;

        void RequireLoose() const {
            if (!IsLoose())
                throw std::runtime_error(
                    "Open individual MPB banks first.");
        }
        void Clear() {
            sourceMode = Mode::None;
            mpbPaths.clear();
            mltPath.clear();
            fpbPath.clear();
            fobPath.clear();
        }
};
} // namespace manatee