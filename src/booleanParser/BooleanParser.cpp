/*
 * Copyright (C) 2016 deipi.com LLC and contributors.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include <algorithm>
#include <iomanip>

#include "AndNode.h"
#include "IdNode.h"
#include "NotNode.h"
#include "SyntacticException.h"
#include "OrNode.h"
#include "XorNode.h"

#include "BooleanParser.h"


BooleanTree::BooleanTree(const std::string& input_)
{
	input = std::make_unique<char[]>(input_.size()+1);
	std::strcpy(input.get(), input_.c_str());
    lexer = std::make_unique<Lexer>(input.get());
	toRPN();
}


void
BooleanTree::Parse()
{
	root = BuildTree();
	if (!stack_output.empty()) {
		Token token = stack_output.back();
		string msj = "'" + token.lexeme + "' not expected";
		throw SyntacticException(msj.c_str());
	}
}


std::unique_ptr<BaseNode>
BooleanTree::BuildTree()
{
	if (stack_output.size() == 1 || stack_output.back().type == TokenType::Id) {
		Token token = stack_output.back();
		stack_output.pop_back();
		string id = token.lexeme;
		return std::make_unique<IdNode>(id);
	}
	/* Error case */
	else if (stack_output.size() == 1 && stack_output.back().type != TokenType::Id) {
		Token token = stack_output.back();
		string msj = "'" + token.lexeme + "' not expected";
		throw SyntacticException(msj.c_str());
	} else {
		Token token = stack_output.back();
		stack_output.pop_back();
		switch(token.type) {
			case TokenType::Not	:
				return std::make_unique<NotNode>(BuildTree());
				break;
			case TokenType::Or:
				return std::make_unique<OrNode>(BuildTree(), BuildTree());
				break;
			case TokenType::And:
				return std::make_unique<AndNode>(BuildTree(), BuildTree());
				break;
			case TokenType::Xor:
				return std::make_unique<XorNode>(BuildTree(), BuildTree());
				break;
			default:
				// Silence compiler switch warning
				break;
		}
	}
	return nullptr;
}


/*
 * Convert to RPN (Reverse Polish notation)
 * with Dijkstra's Shunting-yard algorithm.
 */
void
BooleanTree::toRPN()
{
	currentToken = lexer->NextToken();

	while (currentToken.type != TokenType::EndOfFile) {

		switch (currentToken.type) {

			case TokenType::Id:
				stack_output.push_back(currentToken);
				break;

			case TokenType::LeftParenthesis:
				stack_operator.push_back(currentToken);
				break;

			case TokenType::RightParenthesis:
				while (true) {
					if (!stack_operator.empty()) {
							Token token_back = stack_operator.back();
						if (token_back.type != TokenType::LeftParenthesis) {
							stack_output.push_back(token_back);
							stack_operator.pop_back();
						} else {
							stack_operator.pop_back();
							break;
						}
					} else {
						string msj = ") was expected";
            			throw SyntacticException(msj.c_str());
					}
				}
				break;
			case TokenType::Not:
			case TokenType::Or:
			case TokenType::And:
			case TokenType::Xor:
				while (!stack_operator.empty() && precedence(currentToken.type) >= precedence(stack_operator.back().type)) {
					Token token_back = stack_operator.back();
					stack_operator.pop_back();
					stack_output.push_back(token_back);
				}
				stack_operator.push_back(currentToken);
				break;

			case TokenType::EndOfFile:
				break;
		}
		currentToken = lexer->NextToken();
	}

	while (!stack_operator.empty()) {
		Token tb = stack_operator.back();
		stack_output.push_back(tb);
		stack_operator.pop_back();
	}
}


unsigned
BooleanTree::precedence(TokenType type)
{
	switch (type) {
		case TokenType::Not:
		case TokenType::And:
			return 0; break;
		case TokenType::Xor:
			return 1; break;
			break;
		case TokenType::Or:
			return 2; break;
		default: return 3;
	}
}


void
BooleanTree::PrintTree()
{
	postorder(root.get());
}


void
BooleanTree::postorder(BaseNode* p, int indent)
{
	if(p != nullptr) {
		switch(p->getType()) {
			case AndNodeType:
				if (dynamic_cast<AndNode*>(p)->getLeftNode()) {
					postorder(dynamic_cast<AndNode*>(p)->getLeftNode(), indent+4);
				}
				if (indent) {
					std::cout << std::setw(indent) << ' ';
				}
				cout<< "AND" << "\n ";
				if (dynamic_cast<AndNode*>(p)->getRightNode()) {
					postorder(dynamic_cast<AndNode*>(p)->getRightNode(), indent+4);
				}
				break;
			case OrNodeType:
				if (dynamic_cast<OrNode*>(p)->getLeftNode()) {
					postorder(dynamic_cast<OrNode*>(p)->getLeftNode(), indent+4);
				}
				if (indent) {
					std::cout << std::setw(indent) << ' ';
				}
				cout<< "OR" << "\n ";
				if (dynamic_cast<OrNode*>(p)->getRightNode()) {
					postorder(dynamic_cast<OrNode*>(p)->getRightNode(), indent+4);
				}
				break;
			case NotNodeType:
				if (dynamic_cast<NotNode*>(p)) {
					std::cout << std::setw(indent) << ' ';
					cout<< "NOT" << "\n ";
					postorder(dynamic_cast<NotNode*>(p)->getNode(), indent+4);
				}
				break;
			case XorNodeType:
				if (dynamic_cast<XorNode*>(p)->getLeftNode()) {
					postorder(dynamic_cast<XorNode*>(p)->getLeftNode(), indent+4);
				}
				if (indent) {
					std::cout << std::setw(indent) << ' ';
				}
				cout<< "XOR" << "\n ";
				if (dynamic_cast<XorNode*>(p)->getRightNode()) {
					postorder(dynamic_cast<XorNode*>(p)->getRightNode(), indent+4);
				}
				break;
			case IdNodeType:
				std::cout << std::setw(indent) << ' ';
				if (dynamic_cast<IdNode*>(p)) {
					cout<< dynamic_cast<IdNode*>(p)->getId() << "\n ";
				}
				break;
		}
	}
}
