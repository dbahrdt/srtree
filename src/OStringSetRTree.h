#pragma once

#include <liboscar/StaticOsmCompleter.h>
#include <srtree/SRTree.h>

struct OStringSetRTree {
	using Tree = srtree::StringSetRTree<12, 32>;
	
	using GeometryTraits = Tree::GeometryTraits;
	
	using SignatureTraits = Tree::SignatureTraits;
	using Signature = Tree::Signature;
	
	struct State {
		Tree tree;
		std::vector<Tree::ItemNode const *> itemNodes;
	};
	struct CreationState {
		///in ghId order!
		std::vector<sserialize::ItemIndex> regionStrIds;
		std::vector<sserialize::ItemIndex> cellStrIds;
		sserialize::SimpleBitVector processedItems;
		SignatureTraits::Combine combine;
	public:
		CreationState(SignatureTraits const & straits) : combine(straits.combine()) {}
	};
public:
	OStringSetRTree(std::shared_ptr<liboscar::Static::OsmCompleter> cmp) :
	cmp(cmp), cstate(state.tree.straits())
	{}
public:
	void setCheck(bool check) { this->check = check; }
public:
	void init();
	void create();
public:
	void serialize(sserialize::UByteArrayAdapter & treeData, sserialize::UByteArrayAdapter & traitsData);
public:
	bool equal(sserialize::UByteArrayAdapter treeData, sserialize::UByteArrayAdapter traitsData);
public:
	void test();
	sserialize::ItemIndex cellStrIds(uint32_t cellId);
	sserialize::ItemIndex itemStrIds(uint32_t itemId);
	std::string normalize(std::string const & str);
private:
	sserialize::ItemIndex matchingStrings(std::string const & str, bool prefixMatch);
	sserialize::ItemIndex matchingItems(typename Tree::GeometryMatchPredicate & gmp, typename Tree::SignatureMatchPredicate & smp);
public:
	std::shared_ptr<liboscar::Static::OsmCompleter> cmp;
	State state;
	CreationState cstate;
	bool check{false};
	
};
