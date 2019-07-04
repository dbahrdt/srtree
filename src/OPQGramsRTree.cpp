#include "OPQGramsRTree.h"

#include <sserialize/strings/unicode_case_functions.h>

#include <liboscar/KVStats.h>


void
OPQGramsRTree::create() {
	sserialize::ProgressInfo pinfo;
	
	pinfo.begin(cmp->store().size(), "Gathering candidate strings");
	for(uint32_t i(0), s(cmp->store().size()); i < s; ++i) {
		auto item = cmp->store().kvItem(i);
		for(uint32_t j(0), js(item.size()); j < js; ++j) {
			std::string token = "@" + item.key(j) + ":" + item.value(j);
			state.tree.straits().add( normalize(token) );
		}
		pinfo(i);
	}
	pinfo.end();
	
	pinfo.begin(cmp->store().geoHierarchy().regionSize(), "Computing region signatures");
	for(uint32_t regionId(0), rs(cmp->store().geoHierarchy().regionSize()); regionId < rs; ++regionId) {
		cstate.regionSignatures.push_back(itemSignature(cmp->store().geoHierarchy().ghIdToStoreId(regionId)));
		pinfo(regionId);
	}
	pinfo.end();
	
	pinfo.begin(cmp->store().geoHierarchy().cellSize(), "Computing cell signatures");
	for(uint32_t cellId(0), cs(cmp->store().geoHierarchy().cellSize()); cellId < cs; ++cellId) {
		cstate.cellSignatures.push_back(cellSignature(cellId));
		pinfo(cellId);
	}
	pinfo.end();
	
	state.itemNodes.resize(cmp->store().size(), 0);
	
	uint32_t numProcItems = 0;
	
	pinfo.begin(cmp->store().size(), "Inserting items");
	for(uint32_t cellId(0), cs(cmp->store().geoHierarchy().cellSize()); cellId < cs; ++cellId) {
		sserialize::ItemIndex cellItems = cmp->indexStore().at(cmp->store().geoHierarchy().cellItemsPtr(cellId));
		for(uint32_t itemId : cellItems) {
			if (cstate.processedItems.isSet(itemId)) {
				continue;
			}
			cstate.processedItems.set(itemId);
			auto b = cmp->store().geoShape(itemId).boundary();
			auto isig = itemSignature(itemId);
			for(auto x : cmp->store().cells(itemId)) {
				isig = cstate.combine(isig, cstate.cellSignatures.at(x));
			}
			state.itemNodes.at(itemId) = state.tree.insert(b, isig, itemId);
			
			++numProcItems;
			pinfo(numProcItems);
		}
		if (check && !state.tree.checkConsistency()) {
			throw sserialize::CreationException("Tree failed consistency check!");
		}
	}
	pinfo.end();
	
	if (check && !state.tree.checkConsistency()) {
		throw sserialize::CreationException("Tree failed consistency check!");
	}
	
	pinfo.begin(1, "Calculating signatures");
	state.tree.recalculateSignatures();
	pinfo.end();
}

NO_OPTIMIZE
void
OPQGramsRTree::test() {
	if (!state.tree.checkConsistency()) {
		std::cerr << "Tree is not consistent" << std::endl;
		return;
	}
	
	//check if tree returns all elements of each cell
	sserialize::ProgressInfo pinfo;
	auto const & gh = cmp->store().geoHierarchy();
	pinfo.begin(gh.cellSize(), "Testing spatial constraint");
	for(uint32_t cellId(0), cs(gh.cellSize()); cellId < cs; ++cellId) {
		auto cellItems = cmp->indexStore().at(gh.cellItemsPtr(cellId));
		auto cb = gh.cellBoundary(cellId);
		
		std::vector<uint32_t> tmp;
		state.tree.find(state.tree.gtraits().mayHaveMatch(cb), std::back_inserter(tmp));
		std::sort(tmp.begin(), tmp.end());
		sserialize::ItemIndex result(std::move(tmp));
		
		if ( (cellItems - result).size() ) {
			std::cout << "Incorrect result for cell " << cellId << std::endl;
		}
	}
	pinfo.end();
	//now check the most frequent key:value pair combinations
	std::cout << "Computing store kv stats..." << std::flush;
	auto kvstats = liboscar::KVStats(cmp->store()).stats(sserialize::ItemIndex(sserialize::RangeGenerator<uint32_t>(0, cmp->store().size())), 0);
	std::cout << "done" << std::endl;
	auto topkv = kvstats.topkv(100, [](auto const & a, auto const & b) {
		return a.valueCount < b.valueCount;
	});
	std::vector<std::string> kvstrings;
	for(auto const & x : topkv) {
		std::string str = "@";
		str += cmp->store().keyStringTable().at(x.keyId);
		str += ":";
		str += cmp->store().valueStringTable().at(x.valueId);
		kvstrings.push_back( normalize(str) );
	}
	
	uint32_t failedQueries = 0;
	
	auto storeBoundary = cmp->store().boundary();
	pinfo.begin(kvstrings.size(), "Testing string constraint");
	for(std::size_t i(0), s(kvstrings.size()); i < s; ++i) {
		std::string const & str = kvstrings[i];
		sserialize::ItemIndex items = cmp->cqrComplete("\"" + str + "\"").flaten();
		
		auto smp = state.tree.straits().mayHaveMatch(str, 0);
		auto gmp = state.tree.gtraits().mayHaveMatch(storeBoundary);
		
		std::vector<uint32_t> tmp;
		state.tree.find(gmp, smp, std::back_inserter(tmp));
		std::sort(tmp.begin(), tmp.end());
		sserialize::ItemIndex result(std::move(tmp));
		
		if ( (items - result).size() ) {
			std::cout << "Incorrect result for query string " << str << ": " << (items - result).size() << std::endl;
			++failedQueries;

			sserialize::ItemIndex mustResult(matchingItems(gmp, smp));
			if (result != mustResult) {
				std::cout << "Tree does not return all valid items: "<< std::endl;
				std::cout << "Correct result: " << mustResult.size() << std::endl;
				std::cout << "Have result: " << result.size() << std::endl;
				std::cout << "Missing: " << (mustResult - result).size() << std::endl;
				std::cout << "Invalid: " << (result - mustResult).size() << std::endl;
			}
		}
		pinfo(i);
	}
	pinfo.end();
	
	failedQueries = 0;
	pinfo.begin(kvstrings.size(), "Testing string+boundary constraint");
	for(std::size_t i(0), s(kvstrings.size()); i < s; ++i) {
		std::string const & str = kvstrings[i];
		
		auto smp = state.tree.straits().mayHaveMatch(str, 0);
		auto cqr = cmp->cqrComplete("\"" + str + "\"");
		for(uint32_t i(0), s(cqr.cellCount()); i < s; ++i) {
			std::vector<uint32_t> tmp;
			auto gmp = state.tree.gtraits().mayHaveMatch(cmp->store().geoHierarchy().cellBoundary(cqr.cellId(i)));
			state.tree.find(gmp, smp, std::back_inserter(tmp));
			std::sort(tmp.begin(), tmp.end());
			sserialize::ItemIndex result(std::move(tmp));
			
			if ( (cqr.items(i) - result).size() ) {
				std::cout << "Incorrect result for query string " << str << " and cell " << cqr.cellId(i) << std::endl;
				++failedQueries;
				
				sserialize::ItemIndex mustResult(matchingItems(gmp, smp));
				if (result != mustResult) {
					std::cout << "Tree does not return all valid items: "<< std::endl;
					std::cout << "Correct result: " << mustResult.size() << std::endl;
					std::cout << "Have result: " << result.size() << std::endl;
					std::cout << "Missing: " << (mustResult - result).size() << std::endl;
					std::cout << "Invalid: " << (result - mustResult).size() << std::endl;
				}
			}
		}
		
		pinfo(i);
	}
	pinfo.end();
	
	if (failedQueries) {
		std::cout << "There were " << failedQueries << " failed queries out of " << kvstrings.size() << std::endl;
	}
}


void
OPQGramsRTree::serialize(sserialize::UByteArrayAdapter & treeData, sserialize::UByteArrayAdapter & traitsData) {
	state.tree.serialize(treeData);
	traitsData << state.tree.straits() << state.tree.gtraits();
}

OPQGramsRTree::Signature
OPQGramsRTree::cellSignature(uint32_t cellId) {
	auto const & gh = cmp->store().geoHierarchy();
	auto cell = gh.cell(cellId);
	Signature sig;
	for(uint32_t i(0), s(cell.parentsSize()); i < s; ++i) {
		sig = cstate.combine(sig, cstate.regionSignatures.at( cell.parent(i) ) );
	}
	return sig;
}

OPQGramsRTree::Signature
OPQGramsRTree::itemSignature(uint32_t itemId) {
	auto item = cmp->store().at(itemId);
	Signature sig;
	for(uint32_t i(0), s(item.size()); i < s; ++i) {
		std::string token = "@" + item.key(i) + ":" + item.value(i);
		auto tsig = state.tree.straits().signature(normalize(token));
		sig = cstate.combine(sig, tsig);
	}
	return sig;
}

sserialize::ItemIndex
OPQGramsRTree::matchingItems(typename Tree::GeometryMatchPredicate & gmp, typename Tree::SignatureMatchPredicate & smp) {
	std::vector<uint32_t> validItems;
	//check all items directly
	for(uint32_t i(cmp->store().geoHierarchy().regionSize()), s(cmp->store().size()); i < s; ++i) {
		auto x = state.itemNodes.at(i);
		SSERIALIZE_CHEAP_ASSERT(x);
		if (gmp(x->boundary()) && smp( x->payload() ) ) {
			validItems.push_back(x->item());
		}
		SSERIALIZE_CHEAP_ASSERT_EQUAL(i, x->item());
	}
	return sserialize::ItemIndex(std::move(validItems));
}

std::string
OPQGramsRTree::normalize(std::string const & str) {
	return sserialize::unicode_to_lower(str);
}
