#include <algorithm>
#include <iomanip>
#include <iostream>
#include "search.h"
#include "searchspace.h"

using namespace std;

/**
 * @brief Records a bunch of useful statistics from the search,
 * which are printed to std error at the end of the search.
 */
struct SearchStatistics {
    uint64_t nodes;
    uint64_t hashProbes, hashHits, hashScoreCuts;
    uint64_t hashMoveAttempts, hashMoveCuts;
    uint64_t failHighs, firstFailHighs;
    uint64_t qsNodes;
    uint64_t qsFailHighs, qsFirstFailHighs;

    SearchStatistics() {
        reset();
    }

    void reset() {
        nodes = 0;
        hashProbes = hashHits = hashScoreCuts = 0;
        hashMoveAttempts = hashMoveCuts = 0;
        failHighs = firstFailHighs = 0;
        qsNodes = 0;
        qsFailHighs = qsFirstFailHighs = 0;
    }
};

struct SearchPV {
    int pvLength;
    Move pv[MAX_DEPTH+1];

    SearchPV() {
        pvLength = 0;
    }
};

const int FUTILITY_MARGIN[4] = {0,
    MAX_POS_SCORE,
    MAX_POS_SCORE + KNIGHT_VALUE,
    MAX_POS_SCORE + QUEEN_VALUE
};

const int REVERSE_FUTILITY_MARGIN[3] = {0,
    MAX_POS_SCORE,
    MAX_POS_SCORE + 2*PAWN_VALUE
};

static Hash transpositionTable(16);
static SearchParameters searchParams;
static SearchStatistics searchStats;

extern bool isStop;

// Search functions
unsigned int getBestMoveAtDepth(Board *b, MoveList &legalMoves, int depth,
    int &bestScore, SearchPV *pvLine);
int PVS(Board &b, int depth, int alpha, int beta, SearchPV *pvLine);
int quiescence(Board &b, int plies, int alpha, int beta);
int checkQuiescence(Board &b, int plies, int alpha, int beta);

// Search helpers
int probeTT(Board &b, Move &hashed, int depth, int &alpha, int beta, SearchPV *pvLine);
int scoreMate(bool isInCheck, int depth, int alpha, int beta);
double getPercentage(uint64_t numerator, uint64_t denominator);
void printStatistics();

// Other utility functions
Move nextMove(MoveList &moves, ScoreList &scores, unsigned int index);
void changePV(Move best, SearchPV *parent, SearchPV *child);
string retrievePV(SearchPV *pvLine);

void getBestMove(Board *b, int mode, int value, Move *bestMove) {
    searchParams.reset();
    searchStats.reset();
    searchParams.rootMoveNumber = (uint8_t) (b->getMoveNumber());

    int color = b->getPlayerToMove();
    MoveList legalMoves = b->getAllLegalMoves(color);
    *bestMove = legalMoves.get(0);
    
    searchParams.timeLimit = (mode == TIME)
        ? (uint64_t)(MAX_TIME_FACTOR * value) : MAX_TIME;
    searchParams.startTime = ChessClock::now();
    double timeSoFar = getTimeElapsed(searchParams.startTime);
    int bestScore, bestMoveIndex;
    
    int rootDepth = 1;
    do {
        // Reset all search parameters (killers, plies, etc)
        searchParams.reset();
        // For recording the PV
        SearchPV pvLine;
        // Get the index of the best move
        bestMoveIndex = getBestMoveAtDepth(b, legalMoves, rootDepth, bestScore, &pvLine);
        if (bestMoveIndex == -1)
            break;
        // Swap the PV to be searched first next iteration
        legalMoves.swap(0, bestMoveIndex);
        *bestMove = legalMoves.get(0);
        

        // Calculate values for printing
        timeSoFar = getTimeElapsed(searchParams.startTime);
        uint64_t nps = (uint64_t) ((double) searchStats.nodes / timeSoFar);
        string pvStr = retrievePV(&pvLine);
        
        // Output info using UCI protocol
        cout << "info depth " << rootDepth << " score";

        // Print score in mate or centipawns
        if (bestScore >= MATE_SCORE - MAX_DEPTH)
            // If it is our mate, it takes plies / 2 + 1 moves to mate since
            // our move ends the game
            cout << " mate " << (MATE_SCORE - bestScore) / 2 + 1;
        else if (bestScore <= -MATE_SCORE + MAX_DEPTH)
            // If we are being mated, it takes plies / 2 moves since our
            // opponent's move ends the game
            cout << " mate " << (-MATE_SCORE - bestScore) / 2;
        else
            // Scale score into centipawns using our internal pawn value
            cout << " cp " << bestScore * 100 / PAWN_VALUE_EG;

        cout << " time " << (int)(timeSoFar * ONE_SECOND)
             << " nodes " << searchStats.nodes << " nps " << nps
             << " hashfull " << 1000 * transpositionTable.keys / transpositionTable.getSize()
             << " pv " << pvStr << endl;
        
        rootDepth++;
    }
    while ((mode == TIME  && (timeSoFar * ONE_SECOND < value * TIME_FACTOR)
                          && (rootDepth <= MAX_DEPTH))
        || (mode == DEPTH && rootDepth <= value));
    
    printStatistics();
    // Aging for the history heuristic table
    searchParams.ageHistoryTable();
    
    isStop = true;
    cout << "bestmove " << moveToString(*bestMove) << endl;
    return;
}

// returns index of best move in legalMoves
unsigned int getBestMoveAtDepth(Board *b, MoveList &legalMoves, int depth,
        int &bestScore, SearchPV *pvLine) {
    SearchPV line;
    int color = b->getPlayerToMove();
    unsigned int tempMove = -1;
    int score = -MATE_SCORE;
    int alpha = -MATE_SCORE;
    int beta = MATE_SCORE;
    
    for (unsigned int i = 0; i < legalMoves.size(); i++) {
        // Stop condition. If stopping, return search results from incomplete
        // search, if any.
        if (isStop)
            return tempMove;

        Board copy = b->staticCopy();
        copy.doMove(legalMoves.get(i), color);
        searchStats.nodes++;
        
        if (i != 0) {
            searchParams.ply++;
            score = -PVS(copy, depth-1, -alpha-1, -alpha, &line);
            searchParams.ply--;
            if (alpha < score && score < beta) {
                searchParams.ply++;
                score = -PVS(copy, depth-1, -beta, -alpha, &line);
                searchParams.ply--;
            }
        }
        else {
            searchParams.ply++;
            score = -PVS(copy, depth-1, -beta, -alpha, &line);
            searchParams.ply--;
        }

        if (score > alpha) {
            alpha = score;
            tempMove = i;
            changePV(legalMoves.get(i), pvLine, &line);
        }
    }

    bestScore = alpha;

    return tempMove;
}

// Gets a best move to try first when a hash move is not available.
int getBestMoveForSort(Board *b, MoveList &legalMoves, int depth) {
    SearchPV line;
    int color = b->getPlayerToMove();
    int tempMove = -1;
    int score = -MATE_SCORE;
    int alpha = -MATE_SCORE;
    int beta = MATE_SCORE;
    
    for (unsigned int i = 0; i < legalMoves.size(); i++) {
        Board copy = b->staticCopy();
        if(!copy.doPseudoLegalMove(legalMoves.get(i), color))
            continue;
        
        if (i != 0) {
            searchParams.ply++;
            score = -PVS(copy, depth-1, -alpha-1, -alpha, &line);
            searchParams.ply--;
            if (alpha < score && score < beta) {
                searchParams.ply++;
                score = -PVS(copy, depth-1, -beta, -alpha, &line);
                searchParams.ply--;
            }
        }
        else {
            searchParams.ply++;
            score = -PVS(copy, depth-1, -beta, -alpha, &line);
            searchParams.ply--;
        }
        
        if (score > alpha) {
            alpha = score;
            tempMove = i;
        }
    }

    return tempMove;
}

//------------------------------------------------------------------------------
//------------------------------Search functions--------------------------------
//------------------------------------------------------------------------------

// The standard implementation of a null-window PVS search.
// The implementation is fail-hard (score returned must be within [alpha, beta])
int PVS(Board &b, int depth, int alpha, int beta, SearchPV *pvLine) {
    // When the standard search is done, enter quiescence search.
    // Static board evaluation is done there.
    if (depth <= 0) {
        pvLine->pvLength = 0;
        return quiescence(b, 0, alpha, beta);
    }

    if (b.isDraw()) {
        if (0 >= beta)
            return beta;
        if (0 > alpha)
            return 0;
        else
            return alpha;
    }
    
    int prevAlpha = alpha;
    int color = b.getPlayerToMove();


    // Probe the hash table for a match/cutoff
    // If a cutoff or exact score hit occurred, probeTT will return a value
    // other than -INFTY
    // alpha is passed by reference in case a hash move raises alpha but does
    // not cause a cutoff
    Move hashed = NULL_MOVE;
    searchStats.hashProbes++;
    int hashScore = probeTT(b, hashed, depth, alpha, beta, pvLine);
    if (hashScore != -INFTY)
        return hashScore;

    SearchPV line;
    // For PVS, the node is a PV node if beta - alpha > 1 (i.e. not a null window)
    // We do not want to do most pruning techniques on PV nodes
    bool isPVNode = (beta - alpha != 1);
    // Similarly, we do not want to prune if we are in check
    bool isInCheck = b.isInCheck(color);
    // A static evaluation, used to activate null move pruning and futility
    // pruning
    int staticEval = (color == WHITE) ? b.evaluate() : -b.evaluate();
    

    // Null move reduction/pruning: if we are in a position good enough that
    // even after passing and giving our opponent a free turn, we still exceed
    // beta, then simply return beta
    // Only if doing a null move does not leave player in check
    // Do not do NMR if the side to move has only pawns
    // Do not do more than 2 null moves in a row
    if (depth >= 3 && !isPVNode && !isInCheck && searchParams.nullMoveCount < 2
                   && staticEval >= beta && b.getNonPawnMaterial(color)) {
        int reduction;
        if (depth >= 11)
            reduction = 4;
        else if (depth >= 6)
            reduction = 3;
        else
            reduction = 2;
        // Reduce more if we are further ahead, but do not let NMR descend
        // directly into q-search
        reduction = min(depth - 2, reduction + (staticEval - beta) / PAWN_VALUE);

        b.doNullMove();
        searchParams.nullMoveCount++;
        searchParams.ply++;
        int nullScore = -PVS(b, depth-1-reduction, -beta, -alpha, &line);
        searchParams.ply--;
        if (nullScore >= beta) {
            b.doNullMove();
            searchParams.nullMoveCount--;
            return beta;
        }
        
        // Undo the null move
        b.doNullMove();
        searchParams.nullMoveCount--;
    }


    // Reverse futility pruning
    // If we are already doing really well and it's our turn, our opponent
    // probably wouldn't have let us get here (a form of the null-move observation
    // adapted to low depths)
    if (!isPVNode && !isInCheck && (depth <= 2 && staticEval - REVERSE_FUTILITY_MARGIN[depth] >= beta)
     && b.getNonPawnMaterial(color))
        return beta;


    SearchSpace ss(&b, color, depth, isPVNode, isInCheck, &searchParams);
    // Generate and sort all pseudo-legal moves
    ss.generateMoves(hashed);


    Move toHash = NULL_MOVE;
    // separate counter only incremented when valid move is searched
    unsigned int movesSearched = (hashed == NULL_MOVE) ? 0 : 1;
    int score = -INFTY;
    for (Move m = ss.nextMove(); m != NULL_MOVE;
              m = ss.nextMove()) {
        // Check for a timeout
        double timeSoFar = getTimeElapsed(searchParams.startTime);
        if (timeSoFar * ONE_SECOND > searchParams.timeLimit)
            isStop = true;
        // Stop condition to help break out as quickly as possible
        if (isStop)
            return -INFTY;


        // Futility pruning
        // If we are already a decent amount of material below alpha, a quiet
        // move probably won't raise our prospects much, so don't bother
        // q-searching it.
        // TODO may fail low in some stalemate cases
        if(depth <= 3 && staticEval <= alpha - FUTILITY_MARGIN[depth]
        && ss.nodeIsReducible() && !isCapture(m) && abs(alpha) < QUEEN_VALUE
        && !isPromotion(m) && !b.isCheckMove(m, color)) {
            score = alpha;
            continue;
        }


        // Futility pruning using SEE
        /*if(!isPVNode && depth == 1 //&& staticEval <= alpha - MAX_POS_SCORE
        && !isInCheck && abs(alpha) < QUEEN_VALUE && !isCapture(m) && !isPromotion(m)
        && !b.isCheckMove(m, color) && b.getExchangeScore(color, m) < 0 && b.getSEE(color, getEndSq(m)) < 0) {
            score = alpha;
            continue;
        }*/


        int reduction = 0;
        Board copy = b.staticCopy();
        if (!copy.doPseudoLegalMove(m, color))
            continue;
        searchStats.nodes++;


        // Late move reduction
        // If we have not raised alpha in the first few moves, we are probably
        // at an all-node. The later moves are likely worse so we search them
        // to a shallower depth.
        // TODO set up an array for reduction values
        if(ss.nodeIsReducible() && !isCapture(m) && depth >= 3 && movesSearched > 2 && alpha <= prevAlpha
        && m != searchParams.killers[searchParams.ply][0] && m != searchParams.killers[searchParams.ply][1]
        && !isPromotion(m) && !copy.isInCheck(color^1)) {
            // Increase reduction with higher depth and later moves, but do
            // not let search descend directly into q-search
            reduction = min(depth - 2,
                (int) (((double) depth - 3.0) / 4.0 + ((double) movesSearched) / 9.5));
        }


        // Null-window search, with re-search if applicable
        if (movesSearched != 0) {
            searchParams.ply++;
            score = -PVS(copy, depth-1-reduction, -alpha-1, -alpha, &line);
            searchParams.ply--;
            // The re-search is always done at normal depth
            if (alpha < score && score < beta) {
                searchParams.ply++;
                score = -PVS(copy, depth-1, -beta, -alpha, &line);
                searchParams.ply--;
            }
        }
        else {
            searchParams.ply++;
            // The first move is always searched at a normal depth
            score = -PVS(copy, depth-1, -beta, -alpha, &line);
            searchParams.ply--;
        }
        
        if (score >= beta) {
            searchStats.failHighs++;
            if (movesSearched == 0)
                searchStats.firstFailHighs++;
            // Hash moves that caused a beta cutoff
            transpositionTable.add(b, depth, m, beta, CUT_NODE, searchParams.rootMoveNumber);
            // Record killer if applicable
            if (!isCapture(m)) {
                // Ensure the same killer does not fill both slots
                if (m != searchParams.killers[searchParams.ply][0]) {
                    searchParams.killers[searchParams.ply][1] = searchParams.killers[searchParams.ply][0];
                    searchParams.killers[searchParams.ply][0] = m;
                }
                // Update the history table
                searchParams.historyTable[color][b.getPieceOnSquare(color, getStartSq(m))][getEndSq(m)]
                    += depth * depth;
                ss.reduceBadHistories(m);
            }
            return beta;
        }
        if (score > alpha) {
            alpha = score;
            toHash = m;
            changePV(m, pvLine, &line);
        }

        movesSearched++;
    }

    // If there were no legal moves
    if (score == -INFTY)
        return scoreMate(ss.isInCheck, depth, alpha, beta);
    
    if (toHash != NULL_MOVE && prevAlpha < alpha && alpha < beta) {
        // Exact scores indicate a principal variation and should always be hashed
        transpositionTable.add(b, depth, toHash, alpha, PV_NODE, searchParams.rootMoveNumber);
        // Update the history table
        if (!isCapture(toHash)) {
            searchParams.historyTable[color][b.getPieceOnSquare(color, getStartSq(toHash))][getEndSq(toHash)]
                += depth * depth;
            ss.reduceBadHistories(toHash);
        }
    }
    // Record all-nodes. The upper bound score can save a lot of search time.
    // No best move can be recorded in a fail-hard framework.
    else if (alpha <= prevAlpha) {
        transpositionTable.add(b, depth, NULL_MOVE, alpha, ALL_NODE, searchParams.rootMoveNumber);
    }

    return alpha;
}

// See if a hash move exists.
int probeTT(Board &b, Move &hashed, int depth, int &alpha, int beta, SearchPV *pvLine) {
    HashEntry *entry = transpositionTable.get(b);
    if (entry != NULL) {
        searchStats.hashHits++;
        // If the node is a predicted all node and score <= alpha, return alpha
        // since score is an upper bound
        // Vulnerable to Type-1 errors
        int hashScore = entry->score;
        uint8_t nodeType = entry->getNodeType();
        if (nodeType == ALL_NODE) {
            if (entry->depth >= depth && hashScore <= alpha) {
                searchStats.hashScoreCuts++;
                return alpha;
            }
        }
        else {
            hashed = entry->m;
            // Only used a hashed score if the search depth was at least
            // the current depth
            if (entry->depth >= depth) {
                // At cut nodes if hash score >= beta return beta since hash
                // score is a lower bound.
                if (nodeType == CUT_NODE && hashScore >= beta) {
                    searchStats.hashScoreCuts++;
                    searchStats.failHighs++;
                    searchStats.firstFailHighs++;
                    return beta;
                }
                // At PV nodes we can simply return the exact score
                /*else if (nodeType == PV_NODE) {
                    searchStats.hashScoreCuts++;
                    return hashScore;
                }*/
            }
            Board copy = b.staticCopy();
            // Sanity check in case of Type-1 hash error
            if (copy.doHashMove(hashed, b.getPlayerToMove())) {
                SearchPV line;
                // If the hash score is unusable and node is not a predicted
                // all-node, we can search the hash move first.
                searchStats.hashMoveAttempts++;
                searchStats.nodes++;
                searchParams.ply++;
                int score = -PVS(copy, depth-1, -beta, -alpha, &line);
                searchParams.ply--;

                if (score >= beta) {
                    searchStats.hashMoveCuts++;
                    return beta;
                }
                if (score > alpha) {
                    alpha = score;
                    changePV(hashed, pvLine, &line);
                }
            }
            else {
                cerr << "Type-1 TT error on " << moveToString(hashed) << endl;
                hashed = NULL_MOVE;
            }
        }
    }
    return -INFTY;
}

// Used to get a score when we have realized that we have no legal moves.
int scoreMate(bool isInCheck, int depth, int alpha, int beta) {
    int score;
    // If we are in check, then checkmate
    if (isInCheck) {
        // Adjust score so that quicker mates are better
        score = (-MATE_SCORE + searchParams.ply);
    }
    else { // else, it is a stalemate
        score = 0;
    }
    if (score >= beta)
        return beta;
    if (score > alpha)
        alpha = score;
    return alpha;
}

/* Quiescence search, which completes all capture and check lines (thus reaching
 * a "quiet" position.)
 * This diminishes the horizon effect and greatly improves playing strength.
 * Delta pruning and static-exchange evaluation are used to reduce the time
 * spent here.
 * The search is done within a fail-hard framework (alpha <= score <= beta)
 */
int quiescence(Board &b, int plies, int alpha, int beta) {
    int color = b.getPlayerToMove();
    if (b.isInCheck(color))
        return checkQuiescence(b, plies, alpha, beta);

    // Stand pat: if our current position is already way too good or way too bad
    // we can simply stop the search here. We first obtain an approximate
    // evaluation for standPat to save time.
    int standPat = (color == WHITE) ? b.evaluateMaterial() : -b.evaluateMaterial();
    if (standPat >= beta + MAX_POS_SCORE)
        return beta;
    
    // delta prune
    if (standPat < alpha - 2 * MAX_POS_SCORE - QUEEN_VALUE)
        return alpha;
    
    // If we do not cut off, we get a more accurate evaluation.
    standPat += (color == WHITE) ? b.evaluatePositional() : -b.evaluatePositional();
    
    if (alpha < standPat)
        alpha = standPat;
    
    if (standPat >= beta)
        return beta;
    
    if (standPat < alpha - MAX_POS_SCORE - QUEEN_VALUE)
        return alpha;
    
    MoveList legalCaptures = b.getPseudoLegalCaptures(color, false);
    ScoreList scores;
    for (unsigned int i = 0; i < legalCaptures.size(); i++) {
        scores.add(b.getMVVLVAScore(color, legalCaptures.get(i)));
    }
    
    int score = -INFTY;
    unsigned int i = 0;
    unsigned int j = 0; // separate counter only incremented when valid move is searched
    for (Move m = nextMove(legalCaptures, scores, i); m != NULL_MOVE;
              m = nextMove(legalCaptures, scores, ++i)) {
        // Delta prune
        if (standPat + b.valueOfPiece(b.getPieceOnSquare(color^1, getEndSq(m))) < alpha - MAX_POS_SCORE)
            continue;
        // Static exchange evaluation pruning
        if (b.getExchangeScore(color, m) < 0 && b.getSEE(color, getEndSq(m)) < -MAX_POS_SCORE)
            continue;

        Board copy = b.staticCopy();
        if (!copy.doPseudoLegalMove(m, color))
            continue;
        
        searchStats.nodes++;
        searchStats.qsNodes++;
        score = -quiescence(copy, plies+1, -beta, -alpha);
        
        if (score >= beta) {
            searchStats.qsFailHighs++;
            if (j == 0)
                searchStats.qsFirstFailHighs++;
            return beta;
        }
        if (score > alpha)
            alpha = score;
        j++;
    }

    MoveList legalPromotions = b.getPseudoLegalPromotions(color);
    for (unsigned int i = 0; i < legalPromotions.size(); i++) {
        Move m = legalPromotions.get(i);

        // Static exchange evaluation pruning
        if(b.getSEE(color, getEndSq(m)) < 0)
            continue;

        Board copy = b.staticCopy();
        if (!copy.doPseudoLegalMove(m, color))
            continue;
        
        searchStats.nodes++;
        searchStats.qsNodes++;
        score = -quiescence(copy, plies+1, -beta, -alpha);
        
        if (score >= beta) {
            searchStats.qsFailHighs++;
            if (j == 0)
                searchStats.qsFirstFailHighs++;
            return beta;
        }
        if (score > alpha)
            alpha = score;
        j++;
    }

    // Checks
    if(plies <= 0) {
        MoveList legalMoves = b.getPseudoLegalChecks(color);

        for (unsigned int i = 0; i < legalMoves.size(); i++) {
            Move m = legalMoves.get(i);

            Board copy = b.staticCopy();
            if (!copy.doPseudoLegalMove(m, color))
                continue;
            
            searchStats.nodes++;
            searchStats.qsNodes++;
            int score = -checkQuiescence(copy, plies+1, -beta, -alpha);
            
            if (score >= beta) {
                searchStats.qsFailHighs++;
                if (j == 0)
                    searchStats.qsFirstFailHighs++;
                alpha = beta;
                return beta;
            }
            if (score > alpha)
                alpha = score;
            j++;
        }
    }

    // TODO This is too slow to be effective
/*    if (score == -INFTY) {
        if (b.isStalemate(color))
            return 0;
    }*/

    return alpha;
}

/*
 * When checks are considered in quiescence, the responses must include all moves,
 * not just captures, necessitating this function.
 */
int checkQuiescence(Board &b, int plies, int alpha, int beta) {
    int color = b.getPlayerToMove();
    MoveList legalMoves = b.getPseudoLegalCheckEscapes(color);

    int score = -INFTY;

    unsigned int j = 0; // separate counter only incremented when valid move is searched
    for (unsigned int i = 0; i < legalMoves.size(); i++) {
        Move m = legalMoves.get(i);

        Board copy = b.staticCopy();
        if (!copy.doPseudoLegalMove(m, color))
            continue;
        
        searchStats.nodes++;
        searchStats.qsNodes++;
        score = -quiescence(copy, plies+1, -beta, -alpha);
        
        if (score >= beta) {
            searchStats.qsFailHighs++;
            if (j == 0)
                searchStats.qsFirstFailHighs++;
            return beta;
        }
        if (score > alpha)
            alpha = score;
        j++;
    }

    // If there were no legal moves
    if (score == -INFTY) {
        // We already know we are in check, so it must be a checkmate
        // Adjust score so that quicker mates are better
        score = (-MATE_SCORE + searchParams.ply + plies);
        if (score >= beta)
            return beta;
        if (score > alpha)
            alpha = score;
    }
    
    return alpha;
}

//------------------------------------------------------------------------------
//------------------------------Other functions---------------------------------
//------------------------------------------------------------------------------

// These functions help to communicate with uci.cpp
void clearTables() {
    transpositionTable.clear();
    searchParams.resetHistoryTable();
}

uint64_t getNodes() {
    return searchStats.nodes;
}

// Retrieves the next move with the highest score, starting from index using a
// partial selection sort. This way, the entire list does not have to be sorted
// if an early cutoff occurs.
Move nextMove(MoveList &moves, ScoreList &scores, unsigned int index) {
    if (index >= moves.size())
        return NULL_MOVE;
    // Find the index of the next best move
    int bestIndex = index;
    int bestScore = scores.get(index);
    for (unsigned int i = index + 1; i < moves.size(); i++) {
        if (scores.get(i) > bestScore) {
            bestIndex = i;
            bestScore = scores.get(bestIndex);
        }
    }
    // Swap the best move to the correct position
    moves.swap(bestIndex, index);
    scores.swap(bestIndex, index);
    return moves.get(index);
}

void changePV(Move best, SearchPV *parent, SearchPV *child) {
    parent->pv[0] = best;
    for (int i = 0; i < child->pvLength; i++) {
        parent->pv[i+1] = child->pv[i];
    }
    parent->pvLength = child->pvLength + 1;
}

// Recover PV for outputting to terminal / GUI using transposition table entries
string retrievePV(SearchPV *pvLine) {
    string pvStr = moveToString(pvLine->pv[0]);
    for (int i = 1; i < pvLine->pvLength; i++) {
        pvStr += " " + moveToString(pvLine->pv[i]);
    }

    return pvStr;
}

// Formats a fraction into a percentage value (0 to 100) for printing
double getPercentage(uint64_t numerator, uint64_t denominator) {
    if (denominator == 0)
        return 0;
    uint64_t tenThousandths = (numerator * 10000) / denominator;
    double percent = ((double) tenThousandths) / 100.0;
    return percent;
}

// Prints the statistics gathered during search
void printStatistics() {
    cerr << setw(22) << "Hash hitrate: " << getPercentage(searchStats.hashHits, searchStats.hashProbes)
         << '%' << " of " << searchStats.hashProbes << " probes" << endl;
    cerr << setw(22) << "Hash score cut rate: " << getPercentage(searchStats.hashScoreCuts, searchStats.hashHits)
         << '%' << " of " << searchStats.hashHits << " hash hits" << endl;
    cerr << setw(22) << "Hash move cut rate: " << getPercentage(searchStats.hashMoveCuts, searchStats.hashMoveAttempts)
         << '%' << " of " << searchStats.hashMoveAttempts << " hash moves" << endl;
    cerr << setw(22) << "First fail high rate: " << getPercentage(searchStats.firstFailHighs, searchStats.failHighs)
         << '%' << " of " << searchStats.failHighs << " fail highs" << endl;
    cerr << setw(22) << "QS Nodes: " << searchStats.qsNodes << " ("
         << getPercentage(searchStats.qsNodes, searchStats.nodes) << '%' << ")" << endl;
    cerr << setw(22) << "QS FFH rate: " << getPercentage(searchStats.qsFirstFailHighs, searchStats.qsFailHighs)
         << '%' << " of " << searchStats.qsFailHighs << " qs fail highs" << endl;
}